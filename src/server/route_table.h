#pragma once

#include "server/http_util.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace cpppools::server {

// 声明式路由表。
//
// 借鉴 Alluxio 的「端点集中注册」思路，但实现完全走 C++ 路线：
//   - Alluxio 用 Java enum 持有 new XxxAction() 实例 + 虚函数派发，依赖 GC 与堆分配；
//   - 这里用 constexpr std::array，表在编译期成型，进程启动零构造开销、零堆分配。
//
// 为什么用线性扫描而不是哈希表：
//   端点只有 9 条，线性扫描的常数远小于哈希计算，且完全没有初始化成本。
//   Java 侧惯用的 HashMap<String, Endpoint> 在这个规模上纯属浪费。
//
// C++ 独有收益：RouteId 是 enum class，配合 switch 全覆盖分发时，
//   新增路由却忘记接 handler 会被 -Wswitch 拦成编译错误，而不是运行期 404。

enum class RouteId {
    // 基础设施端点（无版本前缀）
    kHealth,
    kMetrics,
    // 状态查询 / 自省
    kStats,
    kGetConfig,
    kSchema,
    // 通用行级 CRUD（POST + JSON body，spec §7.1）
    kSelect,
    kInsert,
    kUpdate,
    kDelete,
};

// 单条路由的声明。
// required_params 中前 required_count 项为必需查询参数，缺失即回 400。
struct RouteSpec {
    std::string_view path;
    HttpMethod method;
    RouteId id;
    std::array<std::string_view, 2> required_params;
    std::size_t required_count;
};

// 路由匹配的三种结果。
//
// 关键：把 kMethodMismatch 与 kNoSuchPath 分开，才能正确区分 405 与 404。
// 旧实现对任何非 GET 请求一律回 405，连不存在的路径也是，属于缺陷。
enum class MatchStatus {
    kMatched,
    kMethodMismatch,
    kNoSuchPath,
};

struct RouteMatch {
    MatchStatus status{MatchStatus::kNoSuchPath};
    // 仅当 status == kMatched 时非空。
    const RouteSpec* spec{nullptr};
};

// 按路径与方法查找路由。
RouteMatch find_route(std::string_view path, HttpMethod method);

// 返回第一个缺失的必需参数名；全部齐备时返回 nullopt。
std::optional<std::string_view> first_missing_param(const RouteSpec& spec, std::string_view query);

// 暴露路由表供 /api/v1/get-config 之类的自省用途与测试遍历。
const RouteSpec* route_table_begin();
const RouteSpec* route_table_end();

}  // namespace cpppools::server
