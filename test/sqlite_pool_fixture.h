#pragma once

// 测试共享 fixture：临时 SQLite 文件 + user_scores 表 + Database 连接池。
//
// 中间件不建表（类型级保证），但测试必须自己造被测库 ——
// 这里用裸 sqlite3 连接建 user_scores（与 scripts/init_demo_db.sql 同构）。

#include "cpppools/config/config.h"
#include "cpppools/db/db_connection_pool.h"
#include "db/sqlite_database.h"

#include <sqlite3.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace cpppools::testing {

inline std::string make_temp_db_path(const std::string& tag) {
    const std::string path = (std::filesystem::temp_directory_path() /
        ("cpppools_scoretest_" + tag + "_XXXXXX")).string();
    std::vector<char> buf(path.begin(), path.end());
    buf.push_back('\0');
    const int fd = mkstemp(buf.data());
    assert(fd >= 0);
    close(fd);
    return std::string(buf.data());
}

// 建 user_scores 表（score 四端点的过渡期数据模型，期 5 删除）。
inline void seed_user_scores_table(const std::string& path) {
    sqlite3* raw = nullptr;
    assert(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    const char* ddl =
        "CREATE TABLE user_scores("
        "  user_id INTEGER PRIMARY KEY,"
        "  score INTEGER NOT NULL,"
        "  updated_at_ms INTEGER NOT NULL"
        ")";
    assert(sqlite3_exec(raw, ddl, nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
}

struct SqlitePoolFixture {
    std::string path;
    std::unique_ptr<db::DbConnectionPool> pool;

    explicit SqlitePoolFixture(
        std::size_t pool_size, const std::string& tag,
        std::chrono::milliseconds idle_probe_threshold = std::chrono::milliseconds{30000}) {
        path = make_temp_db_path(tag);
        seed_user_scores_table(path);

        cpppools::config::DataSourceConfig cfg;
        cfg.name = tag;
        cfg.type = "sqlite";
        cfg.sqlite_path = path;
        const db::ConnectionFactory factory =
            [cfg](std::size_t) -> std::unique_ptr<db::Database> {
                // 每槽位独立连接 + 独立 statement cache（池语义要求）。
                db::DatabaseCreateResult result = db::make_sqlite_database(cfg);
                return std::move(result.db);  // 失败为 nullptr → 池 fail-fast
            };
        db::DbPoolConfig pcfg;
        pcfg.pool_size = pool_size;
        pcfg.backend = "sqlite";
        pool = std::make_unique<db::DbConnectionPool>(tag, factory, pcfg,
                                                      idle_probe_threshold);
    }

    SqlitePoolFixture(const SqlitePoolFixture&) = delete;
    SqlitePoolFixture& operator=(const SqlitePoolFixture&) = delete;

    ~SqlitePoolFixture() {
        pool.reset();
        std::remove(path.c_str());
        std::remove((path + "-wal").c_str());
        std::remove((path + "-shm").c_str());
    }
};

}  // namespace cpppools::testing
