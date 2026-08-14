#pragma once

#include "cpppools/server/api_response.h"

#include <chrono>
#include <string>
#include <string_view>

namespace cpppools::db {
class DataSourceRegistry;
}  // namespace cpppools::db

namespace cpppools::server {

class OwnershipRouter;

// 通用 CRUD handler 的入参上下文。
//
// body 是 JSON 请求体视图（handle_client 按 Content-Length 读全后传入），
// 仅在本次请求处理期内有效。四个 CRUD 统一 POST + JSON（spec §7.1）：
// 值可含任意字符（& = 空格 中文），query string 不胜任。
//
// 集群字段（期2，spec §3.6/§5.4）：fallback = 客户端携带
// X-CppPools-Fallback（attempt 绕行申报）；epoch = X-CppPools-Epoch 头值
//（空串 = 未携带）；router 为归属路由器（单节点模式下恒本地）。
struct CrudApiContext {
    db::DataSourceRegistry& registry;
    std::string_view body;
    std::chrono::milliseconds db_timeout;
    bool fallback{false};
    std::string epoch;
    const OwnershipRouter* router{nullptr};
};

// ===== 四个行级 CRUD + schema 自省（spec §7.2 / §7.4）=====
//
// 统一流程：解析 JSON → registry.route(table, datasource) → 借连接
// → Database CRUD（校验管道在基类内强制执行）→ 映射状态码。
// 响应始终回传实际命中的 datasource，便于客户端确认写入位置。
ApiResponse handle_select(const CrudApiContext& ctx);
ApiResponse handle_insert(const CrudApiContext& ctx);
ApiResponse handle_update(const CrudApiContext& ctx);
ApiResponse handle_delete(const CrudApiContext& ctx);
ApiResponse handle_schema(const CrudApiContext& ctx);

}  // namespace cpppools::server
