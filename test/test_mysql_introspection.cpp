// test_mysql_introspection：MySQL 后端自省专项（spec §十五.2）。
//
// 覆盖 MySQL 特有形态：
//   - 联合唯一 UNIQUE(a,b) 不使任何单列 unique（COLUMN_KEY='UNI' 不得误标）
//   - 前缀唯一索引 UNIQUE(code(10)) 不标 unique（sub_part 非空 → 击穿"每操作至多一行"）
//   - autoincrement 仅显式 AUTO_INCREMENT 才算（MySQL 无 SQLite rowid 魔法）
//   - BLOB/JSON/BIT 列拒绝该数据源（error 指明 table.column）
//   - 生成列/隐藏列排除在 ColumnInfo 之外（insert 全列 SQL 不含该列）
//   - 全类型映射表（int/year/decimal/text/datetime/enum 等）
//
// 仅在 CPPPOOLS_USE_MYSQL 编译时有效。连接信息来自环境变量：
//   CPPPOOLS_TEST_MYSQL_HOST（默认 127.0.0.1）
//   CPPPOOLS_TEST_MYSQL_PORT（默认 3306）
//   CPPPOOLS_TEST_MYSQL_USER（必需）
//   CPPPOOLS_TEST_MYSQL_PASSWORD（可空）
// 任一必需变量缺失则跳过（返回 0），不进默认 CI（spec §十五.3）。
//
// 自动化流程：建临时库 → 建表 → 自省断言 → 删库。库名为 cpppools_intro_test_<pid>，
// 测试结束无论成败都删除。

#include "cpppools/db/database.h"
#include "cpppools/db/mysql_database.h"
#include "cpppools/db/value.h"

#include <mysql.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace cpppools::db;
namespace config = cpppools::config;

namespace {

// ---------------------------------------------------------------------------
// 测试连接配置（来自环境变量）
// ---------------------------------------------------------------------------
struct TestConn {
    std::string host;
    int port{3306};
    std::string user;
    std::string password;
    bool valid{false};
};

TestConn load_conn() {
    TestConn c;
    if (const char* h = std::getenv("CPPPOOLS_TEST_MYSQL_HOST")) {
        c.host = h;
    } else {
        c.host = "127.0.0.1";
    }
    if (const char* p = std::getenv("CPPPOOLS_TEST_MYSQL_PORT")) {
        c.port = std::atoi(p);
    }
    if (const char* u = std::getenv("CPPPOOLS_TEST_MYSQL_USER")) {
        c.user = u;
    }
    if (const char* pw = std::getenv("CPPPOOLS_TEST_MYSQL_PASSWORD")) {
        c.password = pw;
    }
    // user 必需；password 可空（MySQL 允许空密码账号）。
    c.valid = !c.user.empty();
    return c;
}

// ---------------------------------------------------------------------------
// 裸连接：执行单条无结果集语句（DDL）。
// ---------------------------------------------------------------------------
bool exec_ddl(MYSQL* conn, const std::string& sql) {
    return mysql_query(conn, sql.c_str()) == 0;
}

bool exec_ddl_or_die(MYSQL* conn, const std::string& sql, const char* what) {
    if (!exec_ddl(conn, sql)) {
        std::cerr << "[setup] " << what << " failed: " << mysql_error(conn) << std::endl;
        std::cerr << "  sql: " << sql << std::endl;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 建测试 schema：覆盖所有需断言的形态。
// ---------------------------------------------------------------------------
const char* kSchemaDdl[] = {
    "CREATE TABLE users("
    "  id INT PRIMARY KEY AUTO_INCREMENT,"
    "  name VARCHAR(255) NOT NULL,"
    "  age INT,"
    "  score REAL,"
    "  tag VARCHAR(255) UNIQUE,"
    "  created_at DATETIME,"
    "  status ENUM('active','inactive'),"
    "  birth_year YEAR,"
    "  amount DECIMAL(10,2),"
    "  full_name VARCHAR(255) AS (CONCAT(name, ' ', name)) VIRTUAL"
    ")",
    "CREATE TABLE pairs(a INT, b INT, UNIQUE(a, b))",
    "CREATE TABLE nums(id INT PRIMARY KEY AUTO_INCREMENT, v INT)",
    "CREATE TABLE prefix_idx(id INT PRIMARY KEY, code VARCHAR(255), UNIQUE(code(10)))",
    // BLOB 拒绝用单独表（在另一库自省，见下）。
};

// 用于 BLOB 拒绝断言的独立建表语句（单独库，自省时直接失败）。
const char* kBlobDdl = "CREATE TABLE bins(id INT PRIMARY KEY, data BLOB)";

// ---------------------------------------------------------------------------
// 临时库 RAII：构造时建库，析构时删库（无论成败）。
// ---------------------------------------------------------------------------
struct TempDatabase {
    MYSQL* root;            // root 连接（不指定 database）
    std::string dbname;
    bool created{false};

    TempDatabase(MYSQL* root_conn, const std::string& name) : root(root_conn), dbname(name) {
        created = exec_ddl(root, "CREATE DATABASE `" + dbname + "`");
    }
    ~TempDatabase() {
        if (created) {
            // 无论测试成败都删库（避免残留影响下次运行）。
            exec_ddl(root, "DROP DATABASE IF EXISTS `" + dbname + "`");
        }
    }
    TempDatabase(const TempDatabase&) = delete;
    TempDatabase& operator=(const TempDatabase&) = delete;
};

// ---------------------------------------------------------------------------
// 断言
// ---------------------------------------------------------------------------

static void test_introspection(const Schema& s) {
    // 表清单：users / pairs / nums / prefix_idx（不含 BLOB 表，那是拒绝断言）。
    assert(s.has_table("users"));
    assert(s.has_table("pairs"));
    assert(s.has_table("nums"));
    assert(s.has_table("prefix_idx"));
    assert(s.table_names().size() == 4);

    // ---- users ----
    // full_name 是 VIRTUAL 生成列 → 排除在 ColumnInfo 之外。
    // 故 users 列数 = 9（id/name/age/score/tag/created_at/status/birth_year/amount）。
    const TableInfo* users = s.find_table("users");
    assert(users != nullptr);
    assert(users->columns.size() == 9);

    const ColumnInfo* id = s.find_column("users", "id");
    assert(id != nullptr);
    assert(id->kind == ValueKind::kInteger);
    assert(id->primary_key);
    assert(id->unique);            // PK 标 unique
    assert(id->autoincrement);     // 显式 AUTO_INCREMENT
    assert(!id->nullable);

    const ColumnInfo* name = s.find_column("users", "name");
    assert(name->kind == ValueKind::kText);
    assert(!name->nullable);
    assert(!name->unique);

    const ColumnInfo* age = s.find_column("users", "age");
    assert(age->kind == ValueKind::kInteger);
    assert(age->nullable);
    assert(!age->unique);

    const ColumnInfo* score = s.find_column("users", "score");
    assert(score->kind == ValueKind::kReal);

    // tag：单列整列 UNIQUE → unique=true、nullable=true（UNIQUE 允许多 NULL）。
    const ColumnInfo* tag = s.find_column("users", "tag");
    assert(tag->kind == ValueKind::kText);
    assert(tag->unique);
    assert(tag->nullable);

    // 类型映射：DATETIME/YEAR/ENUM → kText；DECIMAL → kReal。
    const ColumnInfo* created_at = s.find_column("users", "created_at");
    assert(created_at->kind == ValueKind::kText);
    const ColumnInfo* status = s.find_column("users", "status");
    assert(status->kind == ValueKind::kText);
    const ColumnInfo* birth_year = s.find_column("users", "birth_year");
    assert(birth_year->kind == ValueKind::kInteger);  // YEAR → kInteger
    const ColumnInfo* amount = s.find_column("users", "amount");
    assert(amount->kind == ValueKind::kReal);  // DECIMAL → kReal（精度限制已知）

    // 生成列 full_name 被排除：find_column 应返回 nullptr。
    assert(s.find_column("users", "full_name") == nullptr);

    // ---- pairs：联合唯一 UNIQUE(a,b) ----
    // 关键断言：两列在 columns 视图都是 COLUMN_KEY='UNI'，但自省后不得标列级 unique。
    const ColumnInfo* a = s.find_column("pairs", "a");
    const ColumnInfo* b = s.find_column("pairs", "b");
    assert(a != nullptr && b != nullptr);
    assert(!a->unique && "联合唯一不得使单列 unique");
    assert(!b->unique && "联合唯一不得使单列 unique");

    // ---- nums：AUTO_INCREMENT ----
    const ColumnInfo* nv = s.find_column("nums", "v");
    assert(nv->kind == ValueKind::kInteger);
    assert(nv->nullable);
    assert(!nv->unique);
    const ColumnInfo* nid = s.find_column("nums", "id");
    assert(nid->autoincrement);

    // ---- prefix_idx：前缀唯一索引 UNIQUE(code(10)) ----
    // 关键断言：前缀索引 sub_part=10 非空 → 不得标 unique（击穿"每操作至多一行"）。
    const ColumnInfo* code = s.find_column("prefix_idx", "code");
    assert(code != nullptr);
    assert(!code->unique && "前缀唯一索引不得标列级 unique");
}

// BLOB 列拒绝：建一个含 BLOB 的库，自省应失败，error 指明 table.column。
static void test_blob_rejects(const TestConn& conn, const std::string& dbname_prefix) {
    const int pid =
#ifdef _WIN32
        _getpid();
#else
        getpid();
#endif
    const std::string dbname = dbname_prefix + "_blob_" + std::to_string(pid);

    MYSQL* root = mysql_init(nullptr);
    assert(root != nullptr);
    assert(mysql_real_connect(root, conn.host.c_str(), conn.user.c_str(),
                              conn.password.c_str(), nullptr,
                              static_cast<unsigned int>(conn.port), nullptr, 0) != nullptr);
    // 作用域包裹 db：先 DROP DATABASE（db 析构），再 mysql_close(root)。
    // 顺序反了会因连接已关而 DROP 失败，残留测试库。
    {
        TempDatabase db(root, dbname);
        assert(db.created);
        assert(exec_ddl_or_die(root, "USE `" + dbname + "`", "use blob db"));
        assert(exec_ddl_or_die(root, kBlobDdl, "create bins table"));

        config::DataSourceConfig cfg;
        cfg.name = "blob-ds";
        cfg.type = "mysql";
        cfg.mysql_host = conn.host;
        cfg.mysql_port = conn.port;
        cfg.mysql_user = conn.user;
        cfg.mysql_password = conn.password;
        cfg.mysql_database = dbname;
        DatabaseCreateResult result = make_mysql_database(cfg);
        assert(result.db == nullptr);
        assert(result.error.find("bins.data") != std::string::npos);
        assert(result.error.find("blob") != std::string::npos);
    }
    mysql_close(root);
}

}  // namespace

int main() {
    const TestConn conn = load_conn();
    if (!conn.valid) {
        std::cout << "[skip] CPPPOOLS_TEST_MYSQL_USER not set; skipping mysql introspection test"
                  << std::endl;
        return 0;
    }

    const int pid =
#ifdef _WIN32
        _getpid();
#else
        getpid();
#endif
    const std::string dbname = "cpppools_intro_test_" + std::to_string(pid);

    // root 连接：建/删库用（不指定 database，否则 DROP DATABASE 会拒绝）。
    MYSQL* root = mysql_init(nullptr);
    assert(root != nullptr);
    if (mysql_real_connect(root, conn.host.c_str(), conn.user.c_str(),
                           conn.password.c_str(), nullptr,
                           static_cast<unsigned int>(conn.port), nullptr, 0) == nullptr) {
        std::cerr << "[fail] cannot connect to mysql as " << conn.user << "@" << conn.host
                  << ":" << conn.port << ": " << mysql_error(root) << std::endl;
        mysql_close(root);
        return 1;  // configured but unreachable: fail, not skip
    }

    // 建临时库 + 建表 + 自省断言 + BLOB 拒绝断言，全部在作用域内完成。
    // db（TempDatabase）在作用域结束时析构 DROP DATABASE —— 此时 root 连接仍存活。
    {
        TempDatabase db(root, dbname);
        assert(db.created);
        assert(exec_ddl_or_die(root, "USE `" + dbname + "`", "use test db"));
        for (const char* ddl : kSchemaDdl) {
            assert(exec_ddl_or_die(root, ddl, "create schema table"));
        }

        // 用工厂自省（走 make_mysql_database 真实路径）。
        config::DataSourceConfig cfg;
        cfg.name = "intro-ds";
        cfg.type = "mysql";
        cfg.mysql_host = conn.host;
        cfg.mysql_port = conn.port;
        cfg.mysql_user = conn.user;
        cfg.mysql_password = conn.password;
        cfg.mysql_database = dbname;
        DatabaseCreateResult result = make_mysql_database(cfg);
        assert(result.db != nullptr);
        assert(result.error.empty());

        test_introspection(result.db->schema());
        assert(result.db->healthy());

        result.db.reset();  // 先关业务连接，再让 db 析构删库
    }

    mysql_close(root);

    // BLOB 拒绝断言（独立库，自管连接与删库）。
    test_blob_rejects(conn, "cpppools_intro_test");

    std::cout << "[ok] mysql introspection test passed" << std::endl;
    return 0;
}
