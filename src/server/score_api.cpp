#include "server/score_api.h"

#include "cpppools/db/db_connection_pool.h"
#include "server/http_util.h"

#include <sstream>
#include <utility>

namespace cpppools::server {

// 跨模块类型引入：本文件只依赖 db 模块。
// 放在 .cpp 而非 .h：头文件里保持显式限定以表达接口边界，
// 实现文件里用 using 减少噪音。
using db::DbConnGuard;
using db::DbConnectionPool;
using db::DbOpStatus;
using db::ScoreRecord;
using db::ScoreResult;

namespace {

// 参数解析结果：要么拿到整数，要么持有一个待返回的 400 响应。
struct IntParam {
    bool ok{false};
    int value{0};
    ApiResponse error;
};

// 解析必需的整数查询参数。
//
// 三种失败情形都归为 400，但消息各自区分，便于 SDK 侧定位：
//   - 参数缺失
//   - 参数存在但为空（旧实现会把这种情况误判成缺失）
//   - 参数非整数或为负
IntParam require_int_param(std::string_view query, std::string_view key) {
    IntParam result;
    const auto raw = find_query_value(query, key);
    if (!raw.has_value()) {
        std::ostringstream msg;
        msg << "missing required parameter: " << key;
        result.error = make_error_response(400, "Bad Request", msg.str());
        return result;
    }

    const auto parsed = parse_int_non_negative(*raw);
    if (!parsed.has_value()) {
        std::ostringstream msg;
        msg << "invalid " << key << " parameter, expected non-negative integer";
        result.error = make_error_response(400, "Bad Request", msg.str());
        return result;
    }

    result.ok = true;
    result.value = *parsed;
    return result;
}

ApiResponse json_ok(int status_code, std::string status_text, std::string body) {
    ApiResponse resp;
    resp.status_code = status_code;
    resp.status_text = std::move(status_text);
    resp.body = std::move(body);
    return resp;
}

ApiResponse no_content() {
    ApiResponse resp;
    resp.status_code = 204;
    resp.status_text = "No Content";
    // 204 按 RFC 7230 不得携带响应体。
    resp.body.clear();
    return resp;
}

// 把数据层状态映射为 HTTP 响应。
//
// 这是「状态枚举 -> 状态码」的唯一收口点：数据层不知道 HTTP，
// HTTP 层不知道 SQL，两者只通过 DbOpStatus 交接。
ApiResponse map_status(const ScoreResult& result, int success_code, const char* success_text) {
    switch (result.status) {
        case DbOpStatus::kOk:
            if (success_code == 204) {
                return no_content();
            }
            return json_ok(success_code, success_text, serialize_score_record(result.record));
        case DbOpStatus::kNotFound:
            return make_error_response(404, "Not Found", "score not found");
        case DbOpStatus::kAlreadyExists:
            return make_error_response(409, "Conflict", "score already exists");
        case DbOpStatus::kError:
            break;
    }
    // kError：把底层错误透传给调用方，便于排查 MySQL 侧问题。
    const std::string message =
        result.error.empty() ? std::string("database operation failed") : result.error;
    return make_error_response(500, "Internal Server Error", message);
}

const char* const kDbTimeoutMessage = "db connection timeout";

}  // namespace

std::string serialize_score_record(const ScoreRecord& record) {
    std::ostringstream oss;
    oss << "{\"user_id\":" << record.user_id
        << ",\"score\":" << record.score
        << ",\"updated_at_ms\":" << record.updated_at_ms << "}";
    return oss.str();
}

ApiResponse handle_create_score(const ScoreApiContext& ctx) {
    const IntParam user = require_int_param(ctx.query, "user");
    if (!user.ok) {
        return user.error;
    }
    const IntParam score = require_int_param(ctx.query, "score");
    if (!score.ok) {
        return score.error;
    }

    // 校验通过后才借连接：非法请求不消耗池资源。
    // DbConnGuard 保证任何返回路径（含异常）都会归还连接。
    DbConnGuard guard(ctx.db_pool, ctx.db_timeout);
    if (!guard.valid()) {
        return make_error_response(503, "Service Unavailable", kDbTimeoutMessage);
    }

    const ScoreResult result = guard.get()->create_score(user.value, score.value);
    return map_status(result, 201, "Created");
}

ApiResponse handle_get_score(const ScoreApiContext& ctx) {
    const IntParam user = require_int_param(ctx.query, "user");
    if (!user.ok) {
        return user.error;
    }

    DbConnGuard guard(ctx.db_pool, ctx.db_timeout);
    if (!guard.valid()) {
        return make_error_response(503, "Service Unavailable", kDbTimeoutMessage);
    }

    const ScoreResult result = guard.get()->get_score(user.value);
    return map_status(result, 200, "OK");
}

ApiResponse handle_update_score(const ScoreApiContext& ctx) {
    const IntParam user = require_int_param(ctx.query, "user");
    if (!user.ok) {
        return user.error;
    }
    const IntParam score = require_int_param(ctx.query, "score");
    if (!score.ok) {
        return score.error;
    }

    DbConnGuard guard(ctx.db_pool, ctx.db_timeout);
    if (!guard.valid()) {
        return make_error_response(503, "Service Unavailable", kDbTimeoutMessage);
    }

    const ScoreResult result = guard.get()->update_score(user.value, score.value);
    return map_status(result, 200, "OK");
}

ApiResponse handle_delete_score(const ScoreApiContext& ctx) {
    const IntParam user = require_int_param(ctx.query, "user");
    if (!user.ok) {
        return user.error;
    }

    DbConnGuard guard(ctx.db_pool, ctx.db_timeout);
    if (!guard.valid()) {
        return make_error_response(503, "Service Unavailable", kDbTimeoutMessage);
    }

    const ScoreResult result = guard.get()->delete_score(user.value);
    return map_status(result, 204, "No Content");
}

}  // namespace cpppools::server
