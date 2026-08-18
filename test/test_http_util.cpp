#include "cpppools/server/api_response.h"
#include "server/http_util.h"
#include "server/route_table.h"

#include <cassert>
#include <string>
#include <vector>

using namespace cpppools::server;


// http_util 与 route_table 的单元测试。
//
// 改造前这些逻辑全部藏在 http_server.cpp 的匿名 namespace 里，无法被测试触达，
// 558 行业务路由处于零覆盖状态。把纯函数提取到独立编译单元后才有了这个文件。

// ---------- parse_request_line ----------

static void test_parse_request_line_get() {
    const auto line = parse_request_line("GET /api/v1/stats HTTP/1.1\r\nHost: x\r\n\r\n");
    assert(line.has_value());
    assert(line->method == HttpMethod::kGet);
    assert(line->target == "/api/v1/stats");
}

static void test_parse_request_line_post() {
    const auto line = parse_request_line("POST /api/v1/create-score?user=1&score=2 HTTP/1.1\r\n\r\n");
    assert(line.has_value());
    assert(line->method == HttpMethod::kPost);
    assert(line->target == "/api/v1/create-score?user=1&score=2");
}

static void test_parse_request_line_unsupported_method() {
    // PUT / DELETE 等未实现的方法要能被识别出来，而不是解析失败，
    // 这样上层才能回 405 而不是 400。
    const auto line = parse_request_line("PUT /api/v1/get-score HTTP/1.1\r\n\r\n");
    assert(line.has_value());
    assert(line->method == HttpMethod::kUnsupported);
    assert(line->target == "/api/v1/get-score");
}

static void test_parse_request_line_malformed() {
    assert(!parse_request_line("").has_value());
    assert(!parse_request_line("GET\r\n").has_value());       // 缺 target
    assert(!parse_request_line("   \r\n").has_value());
}

static void test_parse_request_line_requires_crlf_terminator() {
    // 安全闸门：缓冲里没看到 CRLF 就说明请求行可能被截断了，
    // 绝不能当作完整请求处理。
    //
    // 为何重要：旧实现有两处“宽容”——没 CRLF 就把整个缓冲当请求行、
    // 没第二个空格就当“缺 HTTP 版本仍可接受”。两者叠加后，一个超长 URL
    // 被 4096 缓冲截断在 "score=98765" 中间，会变成 "score=98" 并通过校验，
    // 导致**持久化错误数据并返回 201**。
    assert(!parse_request_line("GET /api/v1/get-score?user=123").has_value());
    assert(!parse_request_line("POST /api/v1/create-score?user=1&score=98").has_value());

    // 带了终结符就是完整的，即使缺 HTTP 版本也可接受。
    const auto ok = parse_request_line("GET /api/v1/get-score?user=123\r\n");
    assert(ok.has_value());
    assert(ok->target == "/api/v1/get-score?user=123");

    // 单个 LF 也算终结（宽容非标准客户端）。
    assert(parse_request_line("GET /health\n").has_value());
}

// ---------- split_target ----------

static void test_split_target() {
    const auto a = split_target("/api/v1/get-score?user=42");
    assert(a.path == "/api/v1/get-score");
    assert(a.query == "user=42");

    const auto b = split_target("/health");
    assert(b.path == "/health");
    assert(b.query.empty());

    // 只有问号、没有 query 内容。
    const auto c = split_target("/health?");
    assert(c.path == "/health");
    assert(c.query.empty());
}

// ---------- find_query_value ----------

static void test_find_query_value_present() {
    const auto v = find_query_value("user=42&score=99", "score");
    assert(v.has_value());
    assert(*v == "99");
}

static void test_find_query_value_absent_vs_empty() {
    // 这是改造前的一个真实歧义 bug：旧实现对「参数不存在」和
    // 「参数存在但值为空」都返回空字符串，导致 ?user= 被误判成缺参。
    // optional 让两者可区分。
    const auto missing = find_query_value("score=1", "user");
    assert(!missing.has_value());

    const auto empty = find_query_value("user=&score=1", "user");
    assert(empty.has_value());
    assert(empty->empty());
}

static void test_find_query_value_no_prefix_collision() {
    // "user" 不应匹配到 "username"。
    const auto v = find_query_value("username=bob&user=7", "user");
    assert(v.has_value());
    assert(*v == "7");
}

static void test_find_query_value_last_param() {
    const auto v = find_query_value("a=1&b=2&c=3", "c");
    assert(v.has_value());
    assert(*v == "3");
}

static void test_find_query_value_empty_query() {
    assert(!find_query_value("", "user").has_value());
}

// ---------- parse_int_non_negative ----------

static void test_parse_int_non_negative() {
    assert(parse_int_non_negative("0").value() == 0);
    assert(parse_int_non_negative("42").value() == 42);
    assert(parse_int_non_negative("2147483647").value() == 2147483647);

    assert(!parse_int_non_negative("").has_value());
    assert(!parse_int_non_negative("-1").has_value());
    assert(!parse_int_non_negative("abc").has_value());
    assert(!parse_int_non_negative("12abc").has_value());   // 尾部残留字符
    assert(!parse_int_non_negative(" 12").has_value());     // 前导空格不接受
    assert(!parse_int_non_negative("2147483648").has_value());  // 溢出 INT_MAX
    assert(!parse_int_non_negative("99999999999999999999").has_value());
}

// ---------- percentile_nearest_rank ----------

static void test_percentile_edges() {
    const std::vector<double> empty;
    assert(percentile_nearest_rank(empty, 0.95) == 0.0);

    const std::vector<double> one{7.5};
    assert(percentile_nearest_rank(one, 0.50) == 7.5);
    assert(percentile_nearest_rank(one, 0.99) == 7.5);

    const std::vector<double> two{1.0, 2.0};
    assert(percentile_nearest_rank(two, 0.50) == 1.0);
    assert(percentile_nearest_rank(two, 0.99) == 2.0);

    std::vector<double> hundred;
    hundred.reserve(100);
    for (int i = 1; i <= 100; ++i) {
        hundred.push_back(static_cast<double>(i));
    }
    assert(percentile_nearest_rank(hundred, 0.50) == 50.0);
    assert(percentile_nearest_rank(hundred, 0.95) == 95.0);
    assert(percentile_nearest_rank(hundred, 0.99) == 99.0);
}

// ---------- json_escape ----------

static void test_json_escape() {
    assert(json_escape("plain") == "plain");
    assert(json_escape("say \"hi\"") == "say \\\"hi\\\"");
    assert(json_escape("back\\slash") == "back\\\\slash");
    assert(json_escape("line\nbreak") == "line\\nbreak");
    // 控制字符要转成 \u00XX，否则会产出非法 JSON。
    assert(json_escape(std::string(1, '\x01')) == "\\u0001");
}

// ---------- prometheus_label_escape ----------
static void test_prometheus_label_escape() {
    assert(prometheus_label_escape("plain") == "plain");
    assert(prometheus_label_escape("say\"hi\"") == "say\\\"hi\\\"");
    assert(prometheus_label_escape("back\\slash") == "back\\\\slash");
    assert(prometheus_label_escape("line\nbreak") == "line\\nbreak");
    assert(prometheus_label_escape("") == "");
    assert(prometheus_label_escape("a\"b\\c\n") == "a\\\"b\\\\c\\n");
    assert(prometheus_label_escape("pool_1") == "pool_1");
}

// ---------- route_table ----------

static void test_route_match_success() {
    const auto m = find_route("/api/v1/schema", HttpMethod::kGet);
    assert(m.status == MatchStatus::kMatched);
    assert(m.spec != nullptr);
    assert(m.spec->id == RouteId::kSchema);

    const auto sel = find_route("/api/v1/select", HttpMethod::kPost);
    assert(sel.status == MatchStatus::kMatched);
    assert(sel.spec->id == RouteId::kSelect);
}

static void test_route_method_mismatch_is_distinct_from_missing() {
    // 改造前的缺陷：任何非 GET 请求一律回 405，连不存在的路径也是。
    // 路由表让「路径存在但方法不对」与「路径不存在」可区分。
    const auto mismatch = find_route("/api/v1/select", HttpMethod::kGet);
    assert(mismatch.status == MatchStatus::kMethodMismatch);

    const auto missing = find_route("/no/such/path", HttpMethod::kPost);
    assert(missing.status == MatchStatus::kNoSuchPath);
}

static void test_route_write_endpoints_are_post() {
    // 通用 CRUD 统一 POST + JSON body（spec §7.1）。
    assert(find_route("/api/v1/select", HttpMethod::kPost).status == MatchStatus::kMatched);
    assert(find_route("/api/v1/insert", HttpMethod::kPost).status == MatchStatus::kMatched);
    assert(find_route("/api/v1/update", HttpMethod::kPost).status == MatchStatus::kMatched);
    assert(find_route("/api/v1/delete", HttpMethod::kPost).status == MatchStatus::kMatched);
    // 写端点不接受 GET。
    assert(find_route("/api/v1/insert", HttpMethod::kGet).status == MatchStatus::kMethodMismatch);
}

static void test_infra_endpoints_have_no_version_prefix() {
    assert(find_route("/health", HttpMethod::kGet).status == MatchStatus::kMatched);
    assert(find_route("/metrics", HttpMethod::kGet).status == MatchStatus::kMatched);
    // 旧路径已下线。
    assert(find_route("/api/health", HttpMethod::kGet).status == MatchStatus::kNoSuchPath);
    assert(find_route("/api/stats", HttpMethod::kGet).status == MatchStatus::kNoSuchPath);
    assert(find_route("/", HttpMethod::kGet).status == MatchStatus::kNoSuchPath);
}

static void test_required_params_detection() {
    // 通用 CRUD 的参数在 JSON 请求体里，路由层无必需 query 参数 ——
    // first_missing_param 必须恒回 nullopt（否则会在解析 body 前误回 400）。
    const auto m = find_route("/api/v1/insert", HttpMethod::kPost);
    assert(m.status == MatchStatus::kMatched);
    assert(m.spec->required_count == 0);
    assert(!first_missing_param(*m.spec, "").has_value());
    assert(!first_missing_param(*m.spec, "user=1&score=2").has_value());

    const auto health = find_route("/health", HttpMethod::kGet);
    assert(!first_missing_param(*health.spec, "").has_value());
}

static void test_method_mismatch_still_reports_allowed_method() {
    // 405 响应必须带 Allow 头，而路由表本就知道该路径允许什么方法，
    // 所以 kMethodMismatch 时也要把 spec 带回来，不能置 nullptr。
    const auto m = find_route("/api/v1/select", HttpMethod::kGet);
    assert(m.status == MatchStatus::kMethodMismatch);
    assert(m.spec != nullptr);
    assert(m.spec->method == HttpMethod::kPost);

    const auto g = find_route("/api/v1/schema", HttpMethod::kPost);
    assert(g.status == MatchStatus::kMethodMismatch);
    assert(g.spec != nullptr);
    assert(g.spec->method == HttpMethod::kGet);
}

// ---------- build_http_response ----------

static bool has_header(const std::string& raw, const std::string& header) {
    return raw.find(header) != std::string::npos;
}

static void test_response_200_has_content_headers() {
    ApiResponse resp;
    resp.body = "{\"a\":1}";
    const std::string raw = build_http_response(resp);

    assert(raw.compare(0, 15, "HTTP/1.1 200 OK") == 0);
    assert(has_header(raw, "Content-Type: application/json"));
    assert(has_header(raw, "Content-Length: 7"));
    assert(has_header(raw, "Connection: close"));
    // 头与体之间必须是空行分隔。
    assert(has_header(raw, "\r\n\r\n{\"a\":1}"));
}

static void test_response_204_omits_content_length() {
    // RFC 7230 3.3.2：服务端 MUST NOT 在 204 响应中发送 Content-Length。
    ApiResponse resp;
    resp.status_code = 204;
    resp.status_text = "No Content";
    const std::string raw = build_http_response(resp);

    assert(raw.compare(0, 23, "HTTP/1.1 204 No Content") == 0);
    assert(!has_header(raw, "Content-Length"));
    assert(!has_header(raw, "Content-Type"));
    assert(has_header(raw, "Connection: close"));
}

static void test_response_405_carries_allow_header() {
    // RFC 7231 6.5.5：源服务器 MUST 在 405 响应中生成 Allow 头。
    ApiResponse resp = make_error_response(405, "Method Not Allowed", "nope");
    resp.allow = "POST";
    const std::string raw = build_http_response(resp);

    assert(has_header(raw, "Allow: POST"));
}

static void test_response_without_allow_omits_header() {
    const ApiResponse resp = make_error_response(404, "Not Found", "nope");
    const std::string raw = build_http_response(resp);
    assert(!has_header(raw, "Allow:"));
}

static void test_error_body_shape() {
    const ApiResponse resp = make_error_response(409, "Conflict", "score already exists");
    assert(resp.status_code == 409);
    assert(resp.body == "{\"message\":\"score already exists\",\"status_code\":409}");
}

static void test_error_message_is_escaped() {
    // mysql_error() 的输出可能带引号与换行，不转义会产出非法 JSON。
    const ApiResponse resp =
        make_error_response(500, "Internal Server Error", "bad \"table\"\nname");
    assert(resp.body.find("\\\"table\\\"") != std::string::npos);
    assert(resp.body.find("\\n") != std::string::npos);
    assert(resp.body.find('\n') == std::string::npos);  // 不得残留真正的换行
}

static void test_find_content_length_parses_value() {
    // 头区以空行（\r\n\r\n）结束；Content-Length 大小写不敏感（RFC 7230 头名不区分大小写）。
    assert(find_content_length("POST /x HTTP/1.1\r\nContent-Length: 17\r\n\r\n{}") == 17);
    assert(find_content_length("POST /x HTTP/1.1\r\ncontent-length: 5\r\n\r\nabcde") == 5);
    assert(find_content_length("POST /x HTTP/1.1\r\nContent-Length: 0\r\n\r\n") == 0);
    // 无头区终结符：视为请求不完整。
    assert(!find_content_length("POST /x HTTP/1.1\r\nContent-Length: 17").has_value());
    // 无 Content-Length 头：GET 类请求的正常形态。
    assert(!find_content_length("GET /x HTTP/1.1\r\nHost: a\r\n\r\n").has_value());
    // 非法值：非数字 / 负数。
    assert(!find_content_length("POST /x HTTP/1.1\r\nContent-Length: abc\r\n\r\n").has_value());
    assert(!find_content_length("POST /x HTTP/1.1\r\nContent-Length: -1\r\n\r\n").has_value());
}

static void test_find_header_generic() {
    // 大小写不敏感（RFC 7230 §3.2）。
    const auto v1 = find_header("POST /x HTTP/1.1\r\nX-CppPools-Epoch: 5\r\n\r\n",
                                "x-cpppools-epoch");
    assert(v1.has_value() && *v1 == "5");
    // 值的前导空格去掉（OWS，RFC 7230 §3.2.4）。
    const auto v2 = find_header("POST /x HTTP/1.1\r\nX-Foo:    bar  \r\n\r\n", "X-FOO");
    assert(v2.has_value() && *v2 == "bar");
    // 缺失 → nullopt；无头区终结符也视为缺失（请求不完整）。
    assert(!find_header("POST /x HTTP/1.1\r\nHost: a\r\n\r\n", "X-Foo").has_value());
    assert(!find_header("POST /x HTTP/1.1\r\nX-Foo: bar", "X-Foo").has_value());
    // 只匹配完整头名：前缀同名不得误中。
    assert(!find_header("POST /x HTTP/1.1\r\nX-Foo-Extra: 1\r\n\r\n", "X-Foo").has_value());
    // 请求行不是头：首行含冒号也不匹配。
    assert(!find_header("POST /x:a HTTP/1.1\r\n\r\n", "POST /x").has_value());
    // 空值合法（与缺失区分：optional 持有空视图）。
    const auto v3 = find_header("POST /x HTTP/1.1\r\nX-Foo:\r\n\r\n", "X-Foo");
    assert(v3.has_value() && v3->empty());
}

int main() {
    test_parse_request_line_get();
    test_parse_request_line_post();
    test_parse_request_line_unsupported_method();
    test_parse_request_line_malformed();
    test_parse_request_line_requires_crlf_terminator();
    test_split_target();
    test_find_query_value_present();
    test_find_query_value_absent_vs_empty();
    test_find_query_value_no_prefix_collision();
    test_find_query_value_last_param();
    test_find_query_value_empty_query();
    test_parse_int_non_negative();
    test_percentile_edges();
    test_json_escape();
    test_prometheus_label_escape();
    test_route_match_success();
    test_route_method_mismatch_is_distinct_from_missing();
    test_route_write_endpoints_are_post();
    test_infra_endpoints_have_no_version_prefix();
    test_required_params_detection();
    test_method_mismatch_still_reports_allowed_method();
    test_response_200_has_content_headers();
    test_response_204_omits_content_length();
    test_response_405_carries_allow_header();
    test_response_without_allow_omits_header();
    test_error_body_shape();
    test_error_message_is_escaped();
    test_find_content_length_parses_value();
    test_find_header_generic();
    return 0;
}
