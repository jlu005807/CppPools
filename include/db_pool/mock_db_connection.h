#pragma once

#include <string>

// MockDBConnection 支持两种运行模式：
// 1) Mock 模式（默认）：使用可复现的模拟查询结果；
// 2) MySQL 模式：通过 MySQL C API 进行真实查询（需启用 CPPPOOLS_USE_MYSQL）。
class MockDBConnection {
public:
    // Mock 模式构造函数。
    explicit MockDBConnection(int id);

    // MySQL 模式构造函数。
    MockDBConnection(int id,
                     std::string host,
                     unsigned int port,
                     std::string user,
                     std::string password,
                     std::string database,
                     std::string table);

    ~MockDBConnection();

    MockDBConnection(const MockDBConnection&) = delete;
    MockDBConnection& operator=(const MockDBConnection&) = delete;

    int id() const { return id_; }
    bool is_mysql_mode() const { return mysql_mode_; }

    // 连接可用性检测。
    bool healthy();

    // 对上层暴露统一查询接口，内部根据模式分发。
    std::string query_user_score(int user_id);

private:
    std::string query_user_score_mock(int user_id) const;
    std::string query_user_score_mysql(int user_id);

    int id_{0};
    bool mysql_mode_{false};

    std::string host_;
    unsigned int port_{0};
    std::string user_;
    std::string password_;
    std::string database_;
    std::string table_;

#ifdef CPPPOOLS_USE_MYSQL
    struct MYSQL* mysql_{nullptr};
#endif
};
