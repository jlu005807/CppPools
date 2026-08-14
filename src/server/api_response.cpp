#include "cpppools/server/api_response.h"

#include "server/http_util.h"

#include <sstream>

namespace cpppools::server {

ApiResponse make_error_response(int status_code,
                               std::string_view status_text,
                               std::string_view message) {
    ApiResponse resp;
    resp.status_code = status_code;
    resp.status_text = std::string(status_text);

    std::ostringstream body;
    // message 经过 json_escape：它可能承载 mysql_error() 的输出，
    // 属于来自外部系统的动态内容，不转义会产出非法 JSON。
    body << "{\"message\":\"" << json_escape(message) << "\","
         << "\"status_code\":" << status_code << "}";
    resp.body = body.str();
    return resp;
}

void append_http_response(const ApiResponse& resp, std::string& out) {
    // 1xx 与 204 不得携带响应体，也不得发送 Content-Length
    // （RFC 7230 3.3.2："A server MUST NOT send a Content-Length header field in
    //  any response with a status code of 1xx or 204"）。
    const bool body_allowed = resp.status_code != 204 && resp.status_code / 100 != 1;

    out += "HTTP/1.1 ";
    out += std::to_string(resp.status_code);
    out += ' ';
    out += resp.status_text;
    out += "\r\n";
    if (body_allowed) {
        out += "Content-Type: ";
        out += resp.content_type;
        out += "\r\nContent-Length: ";
        out += std::to_string(resp.body.size());
        out += "\r\n";
    }
    if (!resp.allow.empty()) {
        // RFC 7231 6.5.5：405 响应 MUST 包含 Allow 头。
        out += "Allow: ";
        out += resp.allow;
        out += "\r\n";
    }
    for (const auto& [name, value] : resp.extra_headers) {
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }
    out += "Connection: close\r\n\r\n";
    if (body_allowed) {
        out += resp.body;
    }
}

std::string build_http_response(const ApiResponse& resp) {
    std::string out;
    append_http_response(resp, out);
    return out;
}

}  // namespace cpppools::server
