#pragma once

#include <string>
#include <utility>

namespace cpppools::db {

// ScoreRecord：user_scores 资源的领域模型。
//
// 对应 MySQL 表结构（见 docs/mysql_setup.sql）：
//   user_id INT PRIMARY KEY, score INT NOT NULL, updated_at TIMESTAMP
//
// updated_at 统一以 epoch 毫秒暴露，让 mock 与 mysql 两种模式的响应字段完全一致：
//   mock : 写入时取 system_clock
//   mysql: SELECT 时用 UNIX_TIMESTAMP(updated_at) * 1000
struct ScoreRecord {
    int user_id{0};
    int score{0};
    long long updated_at_ms{0};
};

// 数据层操作结果状态。
//
// 设计取舍：这里用「按值返回的状态枚举」而不是抛异常。
// C++ 中异常驱动控制流在热路径上代价较高，而且「记录不存在」属于可预期的正常分支，
// 并非异常情况。上层 HTTP handler 直接把状态映射成 404 / 409。
// 这也是本项目刻意不照搬 Alluxio（Java 侧用 AlluxioRuntimeException 映射状态码）的地方。
enum class DbOpStatus {
    kOk,
    kNotFound,
    kAlreadyExists,
    kError,
};

// 数据层统一返回类型。
//
// 关键设计：数据层只回结构化数据，不回 JSON 字符串。
// 序列化是 HTTP 层的职责——这样同一次查询既能拼 JSON 响应，也能直接用于测试断言，
// 并且能用 status 区分出 404 / 409 这类语义。
struct ScoreResult {
    DbOpStatus status{DbOpStatus::kError};
    ScoreRecord record{};
    std::string error;

    bool ok() const { return status == DbOpStatus::kOk; }

    static ScoreResult success(const ScoreRecord& rec) {
        ScoreResult r;
        r.status = DbOpStatus::kOk;
        r.record = rec;
        return r;
    }

    static ScoreResult not_found() {
        ScoreResult r;
        r.status = DbOpStatus::kNotFound;
        return r;
    }

    static ScoreResult already_exists() {
        ScoreResult r;
        r.status = DbOpStatus::kAlreadyExists;
        return r;
    }

    static ScoreResult failure(std::string message) {
        ScoreResult r;
        r.status = DbOpStatus::kError;
        r.error = std::move(message);
        return r;
    }
};

}  // namespace cpppools::db
