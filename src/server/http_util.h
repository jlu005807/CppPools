#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cpppools::server {

// HTTP 层的纯解析函数。
//
// 为什么单独成文件：改造前这些逻辑都在 http_server.cpp 的匿名 namespace 中，
// 外部无法链接，因此无法编写单元测试。提取到独立编译单元后即可被测试直接调用。
//
// 生命周期契约（重要）：
//   本文件中所有返回 std::string_view 的函数都只是「借用」输入缓冲，不做任何拷贝。
//   这些视图的有效期与传入的原始请求字符串（HttpServer::handle_client 栈上的
//   std::string request）完全一致，仅在 route_request 调用期内有效。
//   禁止把这些视图跨线程传递或存入成员变量。

// 本服务实现的 HTTP 方法。
// kUnsupported 表示「语法合法但本服务未实现」，用于回 405 而非 400。
enum class HttpMethod {
    kGet,
    kPost,
    kUnsupported,
};

struct RequestLine {
    HttpMethod method{HttpMethod::kUnsupported};
    // 请求目标，形如 "/api/v1/select"（尚未切分 query）。
    std::string_view target;
};

// 解析请求首行 "METHOD TARGET HTTP/VERSION"。
// 缺少 method 或 target 时返回 nullopt。
//
// 与旧实现的差别：旧代码用 std::istringstream 承载整个请求（复制一份 4KB 缓冲），
// 这里直接在 string_view 上查找分隔符，解析全程零分配。
std::optional<RequestLine> parse_request_line(std::string_view raw);

struct TargetParts {
    std::string_view path;
    std::string_view query;
};

// 以第一个 '?' 为界切分路径与查询串。无 '?' 时 query 为空。
TargetParts split_target(std::string_view target);

// 从查询串中提取指定 key 的值，不做 URL 解码。
//
// 返回 optional 而非空字符串，是为了区分两种情况：
//   - 参数不存在        -> nullopt
//   - 参数存在但值为空  -> optional 持有空字符串
// 旧实现把两者都返回空字符串，导致 "?user=" 被误判成缺少参数。
std::optional<std::string_view> find_query_value(std::string_view query, std::string_view key);

// 从原始请求文本的头区取指定头的值（头名大小写不敏感，RFC 7230 §3.2）。
// 头区以 "\r\n\r\n" 结束；找不到终结符返回 nullopt（请求不完整）。
// 只匹配完整头名（X-Foo 不中 X-Foo-Extra）；请求行不参与匹配。
// 值去掉两端 OWS；缺失 → nullopt，头存在但值空 → optional 持空视图。
std::optional<std::string_view> find_header(std::string_view raw, std::string_view name);

// 从原始请求文本的头区解析 Content-Length。
// 头区以 "\r\n\r\n" 结束；找不到终结符返回 nullopt（请求不完整，
// 与请求行的完整性闸门同理 —— 防止半截头部被当作完整请求处理）。
// 无 Content-Length 头同样返回 nullopt（GET 请求的正常形态）。
// 值必须是纯非负十进制数，否则 nullopt（400 由调用方负责）。
std::optional<std::size_t> find_content_length(std::string_view raw);

// 解析非负 int。要求整个字符串都是数字，不接受前导空格、正负号与尾部残留字符。
// 越界（> INT_MAX）返回 nullopt。
std::optional<int> parse_int_non_negative(std::string_view s);

// 转义字符串使其可安全嵌入 JSON 字面量。
//
// 必要性：错误消息可能透传后端（SQLite/MySQL）的动态内容，
// 若不转义可能产出非法 JSON。
std::string json_escape(std::string_view s);

// Escape a string for safe use as a Prometheus label value.
// Backslash, double-quote, and newline are the characters that must be
// escaped per the Prometheus exposition format.
std::string prometheus_label_escape(std::string_view s);

// 最近邻秩法分位数（Nearest Rank）。入参必须已升序排序。
// 空输入返回 0.0。
double percentile_nearest_rank(const std::vector<double>& sorted, double p);

}  // namespace cpppools::server
