#include "server/route_table.h"

namespace cpppools::server {

namespace {

// 全部端点的唯一注册点。
//
// 新增端点只需在此追加一行，然后在 http_server.cpp 的 switch 中补上对应分支
// —— 忘记补分支会触发 -Werror=switch 编译错误，不会静默变成 404。
//
// 约定：读操作用 GET，写操作用 POST，参数一律走 query string（不解析请求体）。
// 这与 Alluxio 的 create-file / set-user-quota 一致，同时也让服务端无需实现
// Content-Length 感知的循环 recv。
constexpr std::array<RouteSpec, 8> kRoutes{{
    // 基础设施端点：不带版本前缀，契约独立于业务 API 演进。
    {"/health",                HttpMethod::kGet,  RouteId::kHealth,      {{"", ""}},            0},
    {"/metrics",               HttpMethod::kGet,  RouteId::kMetrics,     {{"", ""}},            0},

    // 状态查询
    {"/api/v1/stats",          HttpMethod::kGet,  RouteId::kStats,       {{"", ""}},            0},
    {"/api/v1/get-config",     HttpMethod::kGet,  RouteId::kGetConfig,   {{"", ""}},            0},

    // CRUD
    {"/api/v1/create-score",   HttpMethod::kPost, RouteId::kCreateScore, {{"user", "score"}},   2},
    {"/api/v1/get-score",      HttpMethod::kGet,  RouteId::kGetScore,    {{"user", ""}},        1},
    {"/api/v1/update-score",   HttpMethod::kPost, RouteId::kUpdateScore, {{"user", "score"}},   2},
    {"/api/v1/delete-score",   HttpMethod::kPost, RouteId::kDeleteScore, {{"user", ""}},        1},
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
