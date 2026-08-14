#include "server/route_table.h"

namespace cpppools::server {

namespace {

// 全部端点的唯一注册点。
//
// 新增端点只需在此追加一行，然后在 http_server.cpp 的 switch 中补上对应分支
// —— 忘记补分支会触发 -Werror=switch 编译错误，不会静默变成 404。
//
// 约定：基础设施/自省端点用 GET；通用 CRUD 统一 POST + JSON body
// （值可含 & = 空格 中文等任意字符，query string 不胜任；insert 还需一次传多列）。
constexpr std::array<RouteSpec, 12> kRoutes{{
    // 基础设施端点：不带版本前缀，契约独立于业务 API 演进。
    {"/health",                HttpMethod::kGet,  RouteId::kHealth,      {{"", ""}},            0},
    {"/metrics",               HttpMethod::kGet,  RouteId::kMetrics,     {{"", ""}},            0},

    // 状态查询 / 自省
    {"/api/v1/stats",          HttpMethod::kGet,  RouteId::kStats,       {{"", ""}},            0},
    {"/api/v1/get-config",     HttpMethod::kGet,  RouteId::kGetConfig,   {{"", ""}},            0},
    {"/api/v1/schema",         HttpMethod::kGet,  RouteId::kSchema,      {{"", ""}},            0},
    {"/api/v1/cluster",        HttpMethod::kGet,  RouteId::kCluster,     {{"", ""}},            0},

    // 通用行级 CRUD：统一 POST + JSON body（参数在请求体里，
    // 无 query 必需参数；值可含任意字符，query string 不胜任）。
    {"/api/v1/select",         HttpMethod::kPost, RouteId::kSelect,      {{"", ""}},            0},
    {"/api/v1/insert",         HttpMethod::kPost, RouteId::kInsert,      {{"", ""}},            0},
    {"/api/v1/update",         HttpMethod::kPost, RouteId::kUpdate,      {{"", ""}},            0},
    {"/api/v1/delete",         HttpMethod::kPost, RouteId::kDelete,      {{"", ""}},            0},

    // 缓存运行时开关（运维事故响应：疑似脏读时立即关缓存，见 spec §8.3）。
    {"/api/v1/cache/disable",  HttpMethod::kPost, RouteId::kCacheDisable, {{"", ""}},           0},
    {"/api/v1/cache/enable",   HttpMethod::kPost, RouteId::kCacheEnable,  {{"", ""}},           0},
}};

}  // namespace

RouteMatch find_route(std::string_view path, HttpMethod method) {
    // 线性扫描：9 条路由下比哈希查找更快，且无需任何初始化。
    const RouteSpec* path_match = nullptr;
    for (const RouteSpec& spec : kRoutes) {
        if (spec.path != path) {
            continue;
        }
        if (spec.method == method) {
            return {MatchStatus::kMatched, &spec};
        }
        // 记下路径命中但方法不对的那条：405 响应必须携带 Allow 头，
        // 而该头的内容就是这条 spec 允许的方法，因此不能丢掉 spec。
        path_match = &spec;
    }
    if (path_match != nullptr) {
        return {MatchStatus::kMethodMismatch, path_match};
    }
    return {MatchStatus::kNoSuchPath, nullptr};
}

std::optional<std::string_view> first_missing_param(const RouteSpec& spec, std::string_view query) {
    for (std::size_t i = 0; i < spec.required_count; ++i) {
        const std::string_view key = spec.required_params[i];
        if (!find_query_value(query, key).has_value()) {
            return key;
        }
    }
    return std::nullopt;
}

const RouteSpec* route_table_begin() {
    return kRoutes.data();
}

const RouteSpec* route_table_end() {
    return kRoutes.data() + kRoutes.size();
}

}  // namespace cpppools::server
