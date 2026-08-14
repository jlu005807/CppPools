#pragma once

#include "cpppools/db/value.h"

#include <string>
#include <vector>

namespace cpppools::db {

// 一列的元信息（启动时自省后端获得）。
struct ColumnInfo {
    std::string name;
    ValueKind kind{ValueKind::kText};
    bool nullable{false};
    bool primary_key{false};
    bool unique{false};        // PK 或 UNIQUE 约束覆盖（where 白名单的依据）
    bool autoincrement{false}; // 自增列（insert 时可缺省，后端生成）
};

struct TableInfo {
    std::string name;
    std::vector<ColumnInfo> columns;
};

// 启动时自省出的 schema 快照：校验管道的白名单来源。
//
// 安全模型的核心：表名/列名在 SQL 里无法参数化，只能拼字符串。
// 校验通过后拼进 SQL 的标识符不是“转义过的客户端输入”，
// 而是“从 schema 里查出来的已知字符串” —— 把不可证明的转义问题
// 换成可证明的集合成员判断。
class Schema {
public:
    void add_table(TableInfo table) { tables_.push_back(std::move(table)); }

    bool has_table(const std::string& name) const {
        return find_table(name) != nullptr;
    }

    // 精确匹配，不做大小写规范化（MySQL 表名在 Linux 区分大小写，
    // macOS 不区分；规范化会引入跨平台行为漂移）。
    const TableInfo* find_table(const std::string& name) const {
        for (const auto& t : tables_) {
            if (t.name == name) {
                return &t;
            }
        }
        return nullptr;
    }

    const ColumnInfo* find_column(const std::string& table, const std::string& column) const {
        const TableInfo* t = find_table(table);
        if (t == nullptr) {
            return nullptr;
        }
        for (const auto& c : t->columns) {
            if (c.name == column) {
                return &c;
            }
        }
        return nullptr;
    }

    std::vector<std::string> table_names() const {
        std::vector<std::string> names;
        names.reserve(tables_.size());
        for (const auto& t : tables_) {
            names.push_back(t.name);
        }
        return names;
    }

private:
    std::vector<TableInfo> tables_;
};

}  // namespace cpppools::db
