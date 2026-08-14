#pragma once

#include "cpppools/db/row_request.h"
#include "cpppools/db/schema.h"
#include "cpppools/db/value.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cpppools::db {

// 查询结果集。
struct ResultSet {
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> rows;
};

// 受影响行数（update/delete）。
using RowCount = std::int64_t;

// insert 的结果：受影响行数 + 生成的自增主键。
// 表无自增主键时 inserted_id 为 nullopt（由后端判定，见决策③）。
struct InsertResult {
    RowCount affected_rows{0};
    std::optional<std::int64_t> inserted_id;
};

// 操作状态（按值返回，项目约定：不用异常做控制流）。
enum class DbStatus {
    kOk,
    kNotFound,         // 保留：后端可用（当前语义下 update/delete 0 行返回 kOk）
    kAlreadyExists,    // 唯一键冲突（insert）
    kInvalidRequest,   // 校验管道拒绝（表/列不存在、类型不符、where 非唯一键等）
    kBackendError,     // 后端错误
};

template <typename T>
struct DbResult {
    DbStatus status{DbStatus::kBackendError};
    T value{};
    std::string message;  // 非 kOk 时非空，可直接对客户端展示（已 JSON 转义由 HTTP 层负责）
};

// 后端数据库的行级 CRUD 抽象（NVI）。
//
// 设计要点：
// - 接口里**没有**建表/删表/改列方法 —— “不能改关系模型”是类型级保证。
// - public select/insert/update/remove 非虚：校验管道在此执行，
//   子类无法绕过（新增后端忘记校验在结构上不可能）。
// - 子类只实现 do_* 与 load 相关钩子；被调用时标识符已过白名单校验，
//   拼 SQL 用的是 schema 里的 ColumnInfo::name，不是客户端原始输入。
//
// 线程模型：单个 Database 实例被一条池化连接独占（同一时刻一个使用者），
// 但实现仍应避免依赖全局可变状态（多线程各自持有实例）。
class Database {
public:
    virtual ~Database() = default;

    // 数据源名（来自配置），用于路由与指标标签。
    const std::string& name() const { return name_; }
    // 后端类型标识（"sqlite" / "mysql" / ...），供日志与调试。
    virtual std::string backend_type() const = 0;

    // 连接可用性检测（探活）。应廉价且尽量不阻塞。
    virtual bool healthy() = 0;

    // per-request 缓存 bypass 提示（cluster-cache spec §5.2 规则 A / §5.4）：
    // 归属门控判定"非 owner 就地处理"或 epoch 不匹配时，handler 在借出的
    // 连接上置位，读完复位。默认 no-op：无缓存的后端无感知。
    // 线程安全前提：池借出的实例被单个请求线程独占（与类注释线程模型一致）。
    // 只关读命中与回填；写的失效不受影响（不变量 I0 由装饰器自己保证）。
    virtual void set_cache_bypass(bool) {}

    const Schema& schema() const { return schema_; }

    // ===== 行级 CRUD（public 非虚：校验管道）=====
    DbResult<ResultSet> select(const SelectRequest& req) {
        const std::string err = validate_where_table(req.table, req.where);
        if (!err.empty()) {
            return invalid<ResultSet>(err);
        }
        for (const auto& col : req.columns) {
            if (schema_.find_column(req.table, col) == nullptr) {
                return invalid<ResultSet>("unknown column \"" + col + "\" in table \"" + req.table + "\"");
            }
        }
        return do_select(req);
    }

    DbResult<InsertResult> insert(const InsertRequest& req) {
        const TableInfo* table = schema_.find_table(req.table);
        if (table == nullptr) {
            return invalid<InsertResult>("unknown table \"" + req.table + "\"");
        }
        if (req.values.empty()) {
            return invalid<InsertResult>("insert requires at least one column value");
        }
        if (has_duplicate_column(req.values)) {
            return invalid<InsertResult>("duplicate column in insert values");
        }
        for (const auto& [col, value] : req.values) {
            const std::string err = validate_column_value(*table, col, value);
            if (!err.empty()) {
                return invalid<InsertResult>(err);
            }
        }
        return do_insert(req);
    }

    DbResult<RowCount> update(const UpdateRequest& req) {
        const TableInfo* table = schema_.find_table(req.table);
        if (table == nullptr) {
            return invalid<RowCount>("unknown table \"" + req.table + "\"");
        }
        if (req.set.empty()) {
            return invalid<RowCount>("update requires at least one column to set");
        }
        const std::string where_err = validate_where_column(*table, req.where);
        if (!where_err.empty()) {
            return invalid<RowCount>(where_err);
        }
        if (has_duplicate_column(req.set)) {
            return invalid<RowCount>("duplicate column in update set");
        }
        for (const auto& [col, value] : req.set) {
            const std::string err = validate_column_value(*table, col, value);
            if (!err.empty()) {
                return invalid<RowCount>(err);
            }
        }
        return do_update(req);
    }

    DbResult<RowCount> remove(const DeleteRequest& req) {
        const TableInfo* table = schema_.find_table(req.table);
        if (table == nullptr) {
            return invalid<RowCount>("unknown table \"" + req.table + "\"");
        }
        const std::string err = validate_where_column(*table, req.where);
        if (!err.empty()) {
            return invalid<RowCount>(err);
        }
        return do_remove(req);
    }

protected:
    // 正常构造：子类在连接建立后自省 schema。
    Database(Schema schema, std::string name)
        : schema_(std::move(schema)), name_(std::move(name)) {}

    // ===== 子类实现 =====
    virtual DbResult<ResultSet> do_select(const SelectRequest&) = 0;
    virtual DbResult<InsertResult> do_insert(const InsertRequest&) = 0;
    virtual DbResult<RowCount> do_update(const UpdateRequest&) = 0;
    virtual DbResult<RowCount> do_remove(const DeleteRequest&) = 0;

    Schema schema_;

private:
    template <typename T>
    static DbResult<T> invalid(std::string message) {
        return DbResult<T>{DbStatus::kInvalidRequest, T{}, std::move(message)};
    }

    // 同一列出现两次是客户端契约违反：拦在校验层（400），
    // 否则后端会以 500 形式暴露。
    template <typename Pairs>
    static bool has_duplicate_column(const Pairs& pairs) {
        for (std::size_t i = 0; i < pairs.size(); ++i) {
            for (std::size_t j = i + 1; j < pairs.size(); ++j) {
                if (pairs[i].first == pairs[j].first) {
                    return true;
                }
            }
        }
        return false;
    }

    // 表存在 + where 列合法（存在 + 唯一 + 值非 NULL + 类型匹配）。
    std::string validate_where_table(const std::string& table, const WhereClause& where) const {
        const TableInfo* t = schema_.find_table(table);
        if (t == nullptr) {
            return "unknown table \"" + table + "\"";
        }
        return validate_where_column(*t, where);
    }

    std::string validate_where_column(const TableInfo& table, const WhereClause& where) const {
        const ColumnInfo* col = nullptr;
        for (const auto& c : table.columns) {
            if (c.name == where.column) {
                col = &c;
                break;
            }
        }
        if (col == nullptr) {
            return "unknown column \"" + where.column + "\" in table \"" + table.name + "\"";
        }
        if (!col->unique) {
            // 核心安全性质：where 必须是 PK/UNIQUE，否则中间件可被用来批量改删。
            return "where column \"" + where.column + "\" must be a primary key or unique";
        }
        if (is_null(where.value)) {
            return "where value must not be NULL";
        }
        if (!value_matches_column(where.value, *col)) {
            return type_error_message(where.value, *col);
        }
        return "";
    }

    // 列值校验：列存在 + NULL 约束 + 严格类型匹配（不做隐式转换，
    // 双后端一致性的前提；唯一放行：整数值可赋给 REAL 列）。
    std::string validate_column_value(const TableInfo& table, const std::string& column,
                                      const Value& value) const {
        const ColumnInfo* col = nullptr;
        for (const auto& c : table.columns) {
            if (c.name == column) {
                col = &c;
                break;
            }
        }
        if (col == nullptr) {
            return "unknown column \"" + column + "\" in table \"" + table.name + "\"";
        }
        if (is_null(value)) {
            if (!col->nullable) {
                return "column \"" + column + "\" does not accept NULL";
            }
            return "";
        }
        if (!value_matches_column(value, *col)) {
            return type_error_message(value, *col);
        }
        return "";
    }

    static bool value_matches_column(const Value& value, const ColumnInfo& col) {
        switch (col.kind) {
            case ValueKind::kInteger:
                return std::holds_alternative<std::int64_t>(value);
            case ValueKind::kReal:
                // 整数可无损表示为实数，放行；小数赋给 INTEGER 列拒绝（不截断）。
                return std::holds_alternative<double>(value) ||
                       std::holds_alternative<std::int64_t>(value);
            case ValueKind::kText:
                return std::holds_alternative<std::string>(value);
        }
        return false;
    }

    static std::string type_error_message(const Value& value, const ColumnInfo& col) {
        std::string given = "null";
        if (std::holds_alternative<std::int64_t>(value)) given = "integer";
        else if (std::holds_alternative<double>(value)) given = "real";
        else if (std::holds_alternative<std::string>(value)) given = "string";
        std::string want = "integer";
        if (col.kind == ValueKind::kReal) want = "real";
        if (col.kind == ValueKind::kText) want = "string";
        return "column \"" + col.name + "\" expects " + want + ", got " + given;
    }

    std::string name_;
};

}  // namespace cpppools::db
