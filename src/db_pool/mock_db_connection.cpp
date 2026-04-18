#include "db_pool/mock_db_connection.h"

#include <chrono>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifdef CPPPOOLS_USE_MYSQL
#if __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#elif __has_include(<mysql.h>)
#include <mysql.h>
#else
#error "CPPPools was configured with MySQL support, but mysql headers were not found."
#endif
#endif

MockDBConnection::MockDBConnection(int id) : id_(id), mysql_mode_(false) {}

MockDBConnection::MockDBConnection(int id,
                                   std::string host,
                                   unsigned int port,
                                   std::string user,
                                   std::string password,
                                   std::string database,
                                   std::string table)
    : id_(id),
      mysql_mode_(true),
      host_(std::move(host)),
      port_(port),
      user_(std::move(user)),
      password_(std::move(password)),
      database_(std::move(database)),
      table_(std::move(table)) {
#ifdef CPPPOOLS_USE_MYSQL
    mysql_ = mysql_init(nullptr);
    if (!mysql_) {
        throw std::runtime_error("mysql_init failed");
    }

    if (!mysql_real_connect(mysql_,
                            host_.c_str(),
                            user_.c_str(),
                            password_.c_str(),
                            database_.c_str(),
                            port_,
                            nullptr,
                            0)) {
        const std::string err = mysql_error(mysql_);
        mysql_close(mysql_);
        mysql_ = nullptr;
        throw std::runtime_error("mysql_real_connect failed: " + err);
    }
#else
    throw std::runtime_error("DB mode is mysql but project was built without CPPPOOLS_USE_MYSQL");
#endif
}

MockDBConnection::~MockDBConnection() {
#ifdef CPPPOOLS_USE_MYSQL
    if (mysql_ != nullptr) {
        mysql_close(mysql_);
        mysql_ = nullptr;
    }
#endif
}

bool MockDBConnection::healthy() {
    if (!mysql_mode_) {
        return true;
    }
#ifdef CPPPOOLS_USE_MYSQL
    return mysql_ != nullptr && mysql_ping(mysql_) == 0;
#else
    return false;
#endif
}

std::string MockDBConnection::query_user_score(int user_id) {
    if (!mysql_mode_) {
        return query_user_score_mock(user_id);
    }
    return query_user_score_mysql(user_id);
}

std::string MockDBConnection::query_user_score_mock(int user_id) const {
    // 模拟远程数据库/网络 I/O 延迟，让压测更接近真实场景。
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // 生成可复现的伪分数，方便教学演示时对比结果。
    const int score = (user_id * 17 + id_ * 3) % 101;

    std::ostringstream oss;
    oss << "{\"user_id\":" << user_id
        << ",\"score\":" << score
        << ",\"db_conn_id\":" << id_ << "}";
    return oss.str();
}

std::string MockDBConnection::query_user_score_mysql(int user_id) {
#ifdef CPPPOOLS_USE_MYSQL
    if (!healthy()) {
        std::ostringstream oss;
        oss << "{\"user_id\":" << user_id
            << ",\"score\":0"
            << ",\"db_conn_id\":" << id_
            << ",\"db_error\":\"connection_unhealthy\"}";
        return oss.str();
    }

    std::ostringstream sql;
    sql << "SELECT score FROM `" << table_ << "` WHERE user_id=" << user_id << " LIMIT 1";

    if (mysql_query(mysql_, sql.str().c_str()) != 0) {
        const unsigned int err_code = mysql_errno(mysql_);
        std::ostringstream oss;
        oss << "{\"user_id\":" << user_id
            << ",\"score\":0"
            << ",\"db_conn_id\":" << mysql_thread_id(mysql_)
            << ",\"db_error_code\":" << err_code << "}";
        return oss.str();
    }

    MYSQL_RES* result = mysql_store_result(mysql_);
    if (!result) {
        const unsigned int err_code = mysql_errno(mysql_);
        std::ostringstream oss;
        oss << "{\"user_id\":" << user_id
            << ",\"score\":0"
            << ",\"db_conn_id\":" << mysql_thread_id(mysql_)
            << ",\"db_error_code\":" << err_code << "}";
        return oss.str();
    }

    int score = 0;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (row != nullptr && row[0] != nullptr) {
        score = std::atoi(row[0]);
    }
    mysql_free_result(result);

    std::ostringstream oss;
    oss << "{\"user_id\":" << user_id
        << ",\"score\":" << score
        << ",\"db_conn_id\":" << mysql_thread_id(mysql_) << "}";
    return oss.str();
#else
    std::ostringstream oss;
    oss << "{\"user_id\":" << user_id
        << ",\"score\":0"
        << ",\"db_conn_id\":" << id_
        << ",\"db_error\":\"mysql_not_enabled\"}";
    return oss.str();
#endif
}
