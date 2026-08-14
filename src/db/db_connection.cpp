#include "cpppools/db/db_connection.h"

#include "cpppools/db/mock_score_store.h"

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

namespace cpppools::db {

DbConnection::DbConnection(int id, MockScoreStore* store)
    : id_(id), mysql_mode_(false), store_(store) {}

DbConnection::DbConnection(int id,
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
                            CLIENT_FOUND_ROWS)) {
        const std::string err = mysql_error(mysql_);
        mysql_close(mysql_);
        mysql_ = nullptr;
        throw std::runtime_error("mysql_real_connect failed: " + err);
    }
#else
    throw std::runtime_error("DB mode is mysql but project was built without CPPPOOLS_USE_MYSQL");
#endif
}

DbConnection::~DbConnection() {
#ifdef CPPPOOLS_USE_MYSQL
    if (mysql_ != nullptr) {
        mysql_close(mysql_);
        mysql_ = nullptr;
    }
#endif
}

bool DbConnection::healthy() {
    if (!mysql_mode_) {
        return true;
    }
#ifdef CPPPOOLS_USE_MYSQL
    return mysql_ != nullptr && mysql_ping(mysql_) == 0;
#else
    return false;
#endif
}

// ===== CRUD 分发层 =====
// mock 模式委派给共享的 MockScoreStore，mysql 模式走真实 SQL。
ScoreResult DbConnection::create_score(int user_id, int score) {
    if (!mysql_mode_) {
        if (store_ == nullptr) {
            return ScoreResult::failure("mock score store is not attached");
        }
        return store_->create(user_id, score);
    }
    return create_score_mysql(user_id, score);
}

ScoreResult DbConnection::get_score(int user_id) {
    if (!mysql_mode_) {
        if (store_ == nullptr) {
            return ScoreResult::failure("mock score store is not attached");
        }
        return store_->get(user_id);
    }
    return get_score_mysql(user_id);
}

ScoreResult DbConnection::update_score(int user_id, int score) {
    if (!mysql_mode_) {
        if (store_ == nullptr) {
            return ScoreResult::failure("mock score store is not attached");
        }
        return store_->update(user_id, score);
    }
    return update_score_mysql(user_id, score);
}

ScoreResult DbConnection::delete_score(int user_id) {
    if (!mysql_mode_) {
        if (store_ == nullptr) {
            return ScoreResult::failure("mock score store is not attached");
        }
        return store_->remove(user_id);
    }
    return delete_score_mysql(user_id);
}

ScoreResult DbConnection::create_score_mysql(int user_id, int score) {
#ifdef CPPPOOLS_USE_MYSQL
    if (!healthy()) {
        return ScoreResult::failure("connection_unhealthy");
    }

    std::ostringstream sql;
    sql << "INSERT INTO `" << table_ << "` (user_id, score) VALUES ("
        << user_id << "," << score << ")";

    if (mysql_query(mysql_, sql.str().c_str()) != 0) {
        const unsigned int err_code = mysql_errno(mysql_);
        // 1062 = ER_DUP_ENTRY。主键冲突语义上就是「已存在」，
        // 应映射成 409 而不是 500。
        if (err_code == 1062) {
            return ScoreResult::already_exists();
        }
        return ScoreResult::failure(mysql_error(mysql_));
    }
    // 插入成功。回读仅为了拿到 DB 生成的 updated_at，
    // 因此回读失败不得推翻“写已成功”这个结论。
    // 若直接 return get_score_mysql(...)，则当其他连接在窗口内删掉该行时，
    // 一条**已经落库成功的 INSERT** 会对外返回 404，状态码就撒谎了。
    const ScoreResult echo = get_score_mysql(user_id);
    if (echo.ok()) {
        return echo;
    }
    return ScoreResult::success(ScoreRecord{user_id, score, 0});
#else
    (void)user_id;
    (void)score;
    return ScoreResult::failure("mysql_not_enabled");
#endif
}

ScoreResult DbConnection::get_score_mysql(int user_id) {
#ifdef CPPPOOLS_USE_MYSQL
    if (!healthy()) {
        return ScoreResult::failure("connection_unhealthy");
    }

    std::ostringstream sql;
    sql << "SELECT score, UNIX_TIMESTAMP(updated_at) * 1000 FROM `" << table_
        << "` WHERE user_id=" << user_id << " LIMIT 1";

    if (mysql_query(mysql_, sql.str().c_str()) != 0) {
        return ScoreResult::failure(mysql_error(mysql_));
    }

    MYSQL_RES* result = mysql_store_result(mysql_);
    if (result == nullptr) {
        return ScoreResult::failure(mysql_error(mysql_));
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    if (row == nullptr) {
        mysql_free_result(result);
        return ScoreResult::not_found();
    }

    ScoreRecord rec;
    rec.user_id = user_id;
    rec.score = (row[0] != nullptr) ? std::atoi(row[0]) : 0;
    rec.updated_at_ms = (row[1] != nullptr) ? std::atoll(row[1]) : 0;
    mysql_free_result(result);
    return ScoreResult::success(rec);
#else
    (void)user_id;
    return ScoreResult::failure("mysql_not_enabled");
#endif
}

ScoreResult DbConnection::update_score_mysql(int user_id, int score) {
#ifdef CPPPOOLS_USE_MYSQL
    if (!healthy()) {
        return ScoreResult::failure("connection_unhealthy");
    }

    std::ostringstream sql;
    sql << "UPDATE `" << table_ << "` SET score=" << score
        << " WHERE user_id=" << user_id;

    if (mysql_query(mysql_, sql.str().c_str()) != 0) {
        return ScoreResult::failure(mysql_error(mysql_));
    }

    // 关键：连接时已传入 CLIENT_FOUND_ROWS，因此 affected_rows 返回的是
    // 「匹配行数」而非「实际改变行数」。
    // 若不带该 flag，把 score 更新为与原值相同时会返回 0，
    // 会被误判成 kNotFound —— 这是 MySQL C API 上一个很容易踩到的坑。
    if (mysql_affected_rows(mysql_) == 0) {
        return ScoreResult::not_found();
    }
    // affected_rows > 0 已证明写成功，同样不能让回读失败改判结果。
    const ScoreResult echo = get_score_mysql(user_id);
    if (echo.ok()) {
        return echo;
    }
    return ScoreResult::success(ScoreRecord{user_id, score, 0});
#else
    (void)user_id;
    (void)score;
    return ScoreResult::failure("mysql_not_enabled");
#endif
}

ScoreResult DbConnection::delete_score_mysql(int user_id) {
#ifdef CPPPOOLS_USE_MYSQL
    if (!healthy()) {
        return ScoreResult::failure("connection_unhealthy");
    }

    // 只需 DELETE + affected_rows 就能判定存在性，不预先回读。
    //
    // 之前的实现会先 SELECT 拿快照再删，但上层 handle_delete_score 返回 204
    // 时会把 record 整个丢掉，于是那次回读纯属浪费：多一次网络往返，
    // 还凭空造出一个“回读后、DELETE 前被他人删除”的竞态窗口。
    std::ostringstream sql;
    sql << "DELETE FROM `" << table_ << "` WHERE user_id=" << user_id;
    if (mysql_query(mysql_, sql.str().c_str()) != 0) {
        return ScoreResult::failure(mysql_error(mysql_));
    }
    if (mysql_affected_rows(mysql_) == 0) {
        return ScoreResult::not_found();
    }
    return ScoreResult::success(ScoreRecord{user_id, 0, 0});
#else
    (void)user_id;
    return ScoreResult::failure("mysql_not_enabled");
#endif
}

}  // namespace cpppools::db
