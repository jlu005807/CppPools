#pragma once

#include "cpppools/db/score_record.h"

#include <string>

namespace cpppools::db {

class MockScoreStore;

// DbConnection 支持两种运行模式：
// 1) Mock 模式（默认）：使用可复现的模拟查询结果；
// 2) MySQL 模式：通过 MySQL C API 进行真实查询（需启用 CPPPOOLS_USE_MYSQL）。
class DbConnection {
public:
    // Mock 模式构造函数。
    // store 为共享的分数存储（非拥有指针，由**调用方**持有 ——
    // factory 化后池不再持有，见 db_connection_pool.h 的 ConnectionFactory）。
    // 传 nullptr 时所有 CRUD 接口都会返回 kError。
    explicit DbConnection(int id, MockScoreStore* store = nullptr);

    // MySQL 模式构造函数。
    DbConnection(int id,
                     std::string host,
                     unsigned int port,
                     std::string user,
                     std::string password,
                     std::string database,
                     std::string table);

    // 虚析构：DbConnectionPool 通过 unique_ptr<DbConnection> 管理连接，
    // 而健康检查需要子类重写 healthy()（如测试里的 FakeConn），
    // 没有虚析构则 delete 基类指针是 UB。
    virtual ~DbConnection();

    DbConnection(const DbConnection&) = delete;
    DbConnection& operator=(const DbConnection&) = delete;

    int id() const { return id_; }
    bool is_mysql_mode() const { return mysql_mode_; }

    // 连接可用性检测。
    //
    // **虚函数**：连接池的健康检查/重建逻辑必须能被测试注入
    // （mock 模式下恒真、MySQL 模式要真连接，都制造不出“探活失败”场景）。
    // 实现约束：应廉价且尽量不阻塞（mock 直接返回；MySQL 是 mysql_ping）。
    virtual bool healthy();

    // ===== CRUD 接口 =====
    // 四个方法均返回结构化的 ScoreResult（而非 JSON 字符串），
    // 序列化交由 HTTP 层负责，这样同一次查询既能拼响应也能直接用于测试断言。
    ScoreResult create_score(int user_id, int score);
    ScoreResult get_score(int user_id);
    ScoreResult update_score(int user_id, int score);
    ScoreResult delete_score(int user_id);

private:
    // MySQL 分支的 CRUD 实现。
    ScoreResult create_score_mysql(int user_id, int score);
    ScoreResult get_score_mysql(int user_id);
    ScoreResult update_score_mysql(int user_id, int score);
    ScoreResult delete_score_mysql(int user_id);

    int id_{0};
    bool mysql_mode_{false};
    // 共享分数存储（仅 mock 模式使用）。非拥有：生命周期由**调用方**管理
    // （必须比本连接活得久；生产里由 main.cpp 的声明顺序保证）。
    MockScoreStore* store_{nullptr};

    std::string host_;
    // maybe_unused：仅 MySQL 分支（#ifdef CPPPOOLS_USE_MYSQL）会读取它，
    // mock 构建下不被使用，不加标记会触发 -Wunused-private-field。
    [[maybe_unused]] unsigned int port_{0};
    std::string user_;
    std::string password_;
    std::string database_;
    std::string table_;

#ifdef CPPPOOLS_USE_MYSQL
    struct MYSQL* mysql_{nullptr};
#endif
};

}  // namespace cpppools::db
