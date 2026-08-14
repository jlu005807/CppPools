#pragma once

#include <string>
#include <string_view>

namespace cpppools::server {

// ApiResponse：handler 的返回值——纯数据，不接触 socket。
//
// 这是分层的关键：handler 只负责「产出状态码 + 响应体」，
// 由 HttpServer 负责拼 HTTP 报文并写回连接。
// 好处是业务逻辑可以完全脱离网络进行单元测试。
struct ApiResponse {
    int status_code{200};
    std::string status_text{"OK"};
    std::string content_type{"application/json; charset=utf-8"};
    std::string body;
    // 405 响应必须携带 Allow 头（RFC 7231 §6.5.5 的 MUST）。
    // 为空表示本次响应不需要该头。
    std::string allow;
};

// 统一的错误响应体：{"message": "...", "status_code": N}
//
// 刻意不照搬 Alluxio 的 {message, statusCode, exception}：
//   - 字段名统一 snake_case，与既有 stats 字段及 Python SDK 惯例一致；
//   - 不回传 exception —— 在 C++ 侧暴露内部类型名属信息泄露。
ApiResponse make_error_response(int status_code,
                               std::string_view status_text,
                               std::string_view message);

// 把 ApiResponse 组装成原始 HTTP/1.1 响应文本，追加到 out。
//
// 接受输出参数而不是返回新字符串，是为了让调用方复用
// ObjectPool 里那个已分配好的 response 缓冲（传入前已 clear() 但保留 capacity）。
void append_http_response(const ApiResponse& resp, std::string& out);

// 便捷包装：返回新构造的响应文本。
//
// 主要供测试使用；204 不得带 body、405 必须带 Allow 这类协议细节
// 需要能被断言覆盖，而不能藏在 HttpServer 的私有成员里。
std::string build_http_response(const ApiResponse& resp);

}  // namespace cpppools::server
