#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace cpppools::db {

// 列/值的逻辑类型（跨后端统一，与 SQL 方言解耦）。
enum class ValueKind {
    kInteger,
    kReal,
    kText,
};

// 数据库单元格的值。monostate = SQL NULL。
//
// 中间件只搬运值、不解释业务含义。BLOB 本版不支持（YAGNI）。
using Value = std::variant<std::monostate, std::int64_t, double, std::string>;

// 值的逻辑类型。
// 契约：调用前必须先 is_null() 检查——NULL 不属于任何列类型，
// 对 NULL 调用本函数的返回值无意义。校验管道与后端列值映射
// 均遵守此顺序（database.h 的两处 validate 即先 is_null 再谈类型）。
inline ValueKind kind_of(const Value& v) {
    struct Visitor {
        ValueKind operator()(std::monostate) const { return ValueKind::kText; }  // 不会被用到
        ValueKind operator()(std::int64_t) const { return ValueKind::kInteger; }
        ValueKind operator()(double) const { return ValueKind::kReal; }
        ValueKind operator()(const std::string&) const { return ValueKind::kText; }
    };
    return std::visit(Visitor{}, v);
}

inline bool is_null(const Value& v) {
    return std::holds_alternative<std::monostate>(v);
}

}  // namespace cpppools::db
