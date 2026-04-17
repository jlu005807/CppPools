#pragma once

#include <string>

// MockDBConnection now supports two runtime modes:
// 1) Mock mode (default): deterministic simulated query
// 2) MySQL mode: real query via MySQL C client (enabled by CPPPOOLS_USE_MYSQL)
class MockDBConnection {
public:
    // Mock mode constructor.
    explicit MockDBConnection(int id);

    // MySQL mode constructor.
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

    // Returns true when connection is usable.
    bool healthy();

    // Unified query API used by upper layers.
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

