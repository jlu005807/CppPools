#pragma once

#include "cpppools/db/score_record.h"
#include "cpppools/server/api_response.h"

#include <chrono>
#include <string>
#include <string_view>

namespace cpppools::db {
class DbConnectionPool;
}  // namespace cpppools::db

namespace cpppools::server {

// handler 的入参上下文。
//
// query 是零拷贝视图，借自原始请求缓冲，仅在本次请求处理期内有效。
struct ScoreApiContext {
    db::DbConnectionPool& db_pool;
    std::string_view query;
    std::chrono::milliseconds db_timeout;
};

// ===== 四个 CRUD handler =====
//
// 统一流程：校验参数 -> 借连接（带超时）-> 调数据层 -> 映射状态码。
// 参数校验一律在借连接之前完成，避免非法请求白占连接池资源。
ApiResponse handle_create_score(const ScoreApiContext& ctx);
ApiResponse handle_get_score(const ScoreApiContext& ctx);
ApiResponse handle_update_score(const ScoreApiContext& ctx);
ApiResponse handle_delete_score(const ScoreApiContext& ctx);

// ===== 供 http_server 复用的构造器 =====

// 把 ScoreRecord 序列化为 JSON 对象文本。
std::string serialize_score_record(const db::ScoreRecord& record);

}  // namespace cpppools::server
