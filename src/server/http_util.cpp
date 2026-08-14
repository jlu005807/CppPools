#include "server/http_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <climits>

namespace cpppools::server {

namespace {

bool is_space(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

// 跳过起始处的空白，返回剩余视图。
std::string_view lstrip(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && is_space(s[i])) {
        ++i;
    }
    return s.substr(i);
}

HttpMethod method_from(std::string_view token) {
    if (token == "GET") {
        return HttpMethod::kGet;
    }
    if (token == "POST") {
        return HttpMethod::kPost;
    }
    return HttpMethod::kUnsupported;
}

}  // namespace

std::optional<RequestLine> parse_request_line(std::string_view raw) {
    // 必须看到行终结符才能认定请求行完整。
    //
    // 这是一道安全闸门，不是格式洁癖：服务端只做一次 4096 字节的 recv，
    // 若请求行被截断（超长 URL，或请求行跨越 TCP 分段），缓冲里就不会有 CRLF。
    // 早先的宽容写法会把“截断后的数字”当成合法参数值：
    //   ...&score=98765  被截成  ...&score=98  → 校验通过 → 写错数据并返回 201。
    // 因此“未见终结符”必须是硬错误，由调用方映射为 400 / 414。
    const auto eol = raw.find_first_of("\r\n");
    if (eol == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view line = raw.substr(0, eol);

    line = lstrip(line);
    if (line.empty()) {
        return std::nullopt;
    }

    const auto sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) {
        return std::nullopt;  // 只有 method，没有 target
    }

    const std::string_view method_token = line.substr(0, sp1);
    std::string_view rest = lstrip(line.substr(sp1 + 1));
    if (rest.empty()) {
        return std::nullopt;
    }

    // target 到下一个空格为止。请求行已确认完整，因此“没有 HTTP 版本”
    // 是客户端真的没发（HTTP/0.9 风格），而不再可能是被截断造成的。
    const auto sp2 = rest.find(' ');
    const std::string_view target = (sp2 == std::string_view::npos) ? rest : rest.substr(0, sp2);
    if (target.empty()) {
        return std::nullopt;
    }

    RequestLine result;
    result.method = method_from(method_token);
    result.target = target;
    return result;
}

TargetParts split_target(std::string_view target) {
    const auto qmark = target.find('?');
    if (qmark == std::string_view::npos) {
        return {target, {}};
    }
    return {target.substr(0, qmark), target.substr(qmark + 1)};
}

std::optional<std::string_view> find_query_value(std::string_view query, std::string_view key) {
    std::size_t begin = 0;
    while (begin < query.size()) {
        const auto amp = query.find('&', begin);
        const std::string_view pair = (amp == std::string_view::npos)
            ? query.substr(begin)
            : query.substr(begin, amp - begin);

        const auto eq = pair.find('=');
        // 严格比较完整 key，避免 "user" 误匹配 "username"。
        const std::string_view k = (eq == std::string_view::npos) ? pair : pair.substr(0, eq);
        if (k == key) {
            // 有 '=' 就返回其后内容（可能为空串），没有 '=' 视为空值。
            // 关键：这里返回的是持有空串的 optional，而不是 nullopt，
            // 从而与「参数完全不存在」区分开。
            return (eq == std::string_view::npos) ? std::string_view{} : pair.substr(eq + 1);
        }

        if (amp == std::string_view::npos) {
            break;
        }
        begin = amp + 1;
    }
    return std::nullopt;
}

std::optional<int> parse_int_non_negative(std::string_view s) {
    if (s.empty()) {
        return std::nullopt;
    }

    // 手写累加而不用 strtol：string_view 不保证以 '\0' 结尾，
    // 交给 strtol 需要先构造一个 std::string，那就白费了零拷贝的功夫。
    long long value = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;  // 拒绝符号、空格与任何非数字字符
        }
        value = value * 10 + (c - '0');
        if (value > INT_MAX) {
            return std::nullopt;  // 提前退出，避免长串输入溢出 long long
        }
    }
    return static_cast<int>(value);
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // 其余控制字符必须转成 \u00XX，否则产出的是非法 JSON。
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(static_cast<unsigned char>(c) >> 4) & 0x0F];
                    out += kHex[static_cast<unsigned char>(c) & 0x0F];
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

double percentile_nearest_rank(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const std::size_t n = sorted.size();
    const std::size_t rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(n)));
    const std::size_t safe_rank = std::max<std::size_t>(rank, 1);
    const std::size_t index = std::min<std::size_t>(safe_rank - 1, n - 1);
    return sorted[index];
}


std::optional<std::size_t> find_content_length(std::string_view raw) {
    // 头区终结符是头部完整的闸门（同 parse_request_line 对请求行的处理）。
    const auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view headers = raw.substr(0, header_end);

    static constexpr std::string_view kLowerName = "content-length";

    // 逐行扫描，头名大小写不敏感（RFC 7230 §3.2）。
    std::size_t pos = 0;
    while (pos < headers.size()) {
        const auto line_end = headers.find("\r\n", pos);
        const std::string_view line =
            headers.substr(pos, line_end == std::string_view::npos ? std::string_view::npos
                                                                   : line_end - pos);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            const std::string_view name = line.substr(0, colon);
            bool match = name.size() == kLowerName.size();
            if (match) {
                // 头名仅 ASCII 字母/数字/连字符，逐字符 tolower 比较安全。
                for (std::size_t i = 0; i < name.size(); ++i) {
                    const char c = name[i];
                    const char lower =
                        (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
                    if (lower != kLowerName[i]) {
                        match = false;
                        break;
                    }
                }
            }
            if (match) {
                // 值：跳过两端空白（OWS），要求全为非负数字。
                std::string_view value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.remove_prefix(1);
                }
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                    value.remove_suffix(1);
                }
                if (value.empty()) {
                    return std::nullopt;
                }
                std::size_t result = 0;
                for (const char c : value) {
                    if (c < '0' || c > '9') {
                        return std::nullopt;
                    }
                    // 溢出保护：超过 1GB 的声明必然非法（服务端有 kMaxBodyBytes 上限）。
                    if (result > (std::size_t{1} << 30)) {
                        return std::nullopt;
                    }
                    result = result * 10 + static_cast<std::size_t>(c - '0');
                }
                return result;
            }
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        pos = line_end + 2;
    }
    return std::nullopt;
}

}  // namespace cpppools::server
