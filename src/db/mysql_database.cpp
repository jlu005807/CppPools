// MySQL 后端：Database NVI 子类的实现。
//
// 仅在编译时启用 CPPPOOLS_USE_MYSQL 时编译。mock 模式（默认 OFF）下，
// 本文件为空 TU —— main 不会注册 mysql 后端，工厂遇到 type=="mysql" 会返回
// "unknown database type"（database_factory.cpp 的既有行为）。
//
// 设计口径见 docs/superpowers/specs/2026-08-15-mysql-backend-design.md。
#ifdef CPPPOOLS_USE_MYSQL

#include "mysql_database.h"

#include "cpppools/db/database_factory.h"
#include "cpppools/db/schema.h"

#include <errmsg.h>
#include <mysql.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cpppools::db {

namespace {

// 连接级超时（秒）。与 SQLite 的 busy_timeout 语义不同：
// SQLite 是库内写锁排队，MySQL 是网络操作超时，但都对齐"不让单次操作无限挂"。
constexpr unsigned int kConnectTimeoutSec = 5;
constexpr unsigned int kReadTimeoutSec = 10;
constexpr unsigned int kWriteTimeoutSec = 10;

// TEXT 列 fetch 缓冲上限。MySQL 的 f->length 是字段声明最大长度（LONGTEXT
// 可达 4GB），按它预分配会 OOM。中间件只搬运单值 TEXT，与缓存 max_entry_bytes
// 同量级 —— 超长值视为异构数据拒绝（对齐中间件「单列单值」语义）。
// 超出时驱动设 s.error=1（MYSQL_DATA_TRUNCATED），read_column 据此报错。
constexpr unsigned long kMaxTextFetchBytes = 65536;

// 失败结果构造：db 为空 + error 非空（对齐项目约定：不用异常做控制流）。
DatabaseCreateResult fail_mysql(std::string error) {
    return {nullptr, std::move(error)};
}

std::string upper_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// ---------------------------------------------------------------------------
// 自省辅助：轻量查询封装
// ---------------------------------------------------------------------------
//
// 自省是一次性启动查询（不在请求路径上），用 mysql_query + store_result
// 的非 prepared 路径即可。标识符（cfg.mysql_database）来自配置而非客户端
// 输入，与 SQLite 自省用 quote_ident 拼 PRAGMA 同构 —— 不存在注入面。
//
// RAII：结果集用完即 mysql_free_result。
class MysqlRows {
public:
    MysqlRows(MYSQL* conn, const std::string& sql, std::string* error)
        : result_(mysql_query(conn, sql.c_str()) == 0
                      ? mysql_store_result(conn)
                      : nullptr) {
        if (result_ == nullptr) {
            // store_result 返回 NULL 可能是真错（查询失败）也可能是无结果集（INSERT 等）。
            // 自省查询都应返回结果集，NULL 在此视为失败。
            *error = "mysql query failed: " + std::string(mysql_error(conn)) +
                     " (sql: " + sql + ")";
        }
    }
    ~MysqlRows() {
        if (result_ != nullptr) {
            mysql_free_result(result_);
        }
    }
    MysqlRows(const MysqlRows&) = delete;
    MysqlRows& operator=(const MysqlRows&) = delete;
    MysqlRows(MysqlRows&&) = delete;
    MysqlRows& operator=(MysqlRows&&) = delete;

    bool ok() const { return result_ != nullptr; }
    MYSQL_RES* raw() { return result_; }

private:
    MYSQL_RES* result_;
};

// ---------------------------------------------------------------------------
// 类型映射（spec §五.1）：MySQL data_type → ValueKind，或拒绝该数据源。
// ---------------------------------------------------------------------------
// 必须穷举：缓存会放大误判后果（误判类型 → 缓存键口径错 → 路由错 + 缓存错）。
// data_type 来自 information_schema.columns，已是小写规范化。
// 返回 false 表示该列类型不可表达（BLOB/JSON/BIT/空间等），拒绝创建。
bool data_type_to_kind(const std::string& data_type, ValueKind* out) {
    // 整数族（含 boolean/bool，实质 tinyint(1)）。
    if (data_type == "tinyint" || data_type == "smallint" || data_type == "mediumint" ||
        data_type == "int" || data_type == "bigint") {
        *out = ValueKind::kInteger;
        return true;
    }
    // year：年份用整数表示。
    if (data_type == "year") {
        *out = ValueKind::kInteger;
        return true;
    }
    // 浮点与定点（DECIMAL/NUMERIC 精度只到 double，已知限制，见 spec §十三）。
    if (data_type == "float" || data_type == "double" || data_type == "decimal" ||
        data_type == "numeric") {
        *out = ValueKind::kReal;
        return true;
    }
    // 文本族。
    if (data_type == "char" || data_type == "varchar" || data_type == "tinytext" ||
        data_type == "text" || data_type == "mediumtext" || data_type == "longtext") {
        *out = ValueKind::kText;
        return true;
    }
    // 日期时间族：用字符串表示（ISO 8601），映射 kText。
    if (data_type == "date" || data_type == "datetime" || data_type == "timestamp" ||
        data_type == "time") {
        *out = ValueKind::kText;
        return true;
    }
    // enum/set：以字符串表示，映射 kText。
    if (data_type == "enum" || data_type == "set") {
        *out = ValueKind::kText;
        return true;
    }
    // 不可表达的类型：二进制与文档型，对齐 SQLite BLOB 亲和拒绝（口径 D7）。
    // bit 是二进制位串；blob/binary/varbinary 是字节串；json 是文档；geometry 是空间类型。
    if (data_type == "bit" || data_type == "blob" || data_type == "tinyblob" ||
        data_type == "mediumblob" || data_type == "longblob" || data_type == "binary" ||
        data_type == "varbinary" || data_type == "json" || data_type == "geometry") {
        return false;
    }
    // 未知类型：保守拒绝而非当 TEXT（缓存放大误判后果）。
    return false;
}

// ---------------------------------------------------------------------------
// 列自省（spec §五.1）
// ---------------------------------------------------------------------------
bool introspect_columns(MYSQL* conn, const std::string& table, TableInfo* out,
                        std::string* error) {
    // 查询本表的列。table_schema 由 make_mysql_database 传入，这里用 SELECT DATABASE()
    // 取当前连接的默认库 —— 与 mysql_real_connect 指定的库一致，避免再传 schema 参数。
    std::vector<char> esc_table(table.size() * 2 + 1);
    const unsigned long esc_len = mysql_real_escape_string(
        conn, esc_table.data(), table.c_str(), static_cast<unsigned long>(table.size()));
    const std::string escaped_table(esc_table.data(), esc_len);
    const std::string sql =
        "SELECT column_name, data_type, is_nullable, column_key, extra "
        "FROM information_schema.columns "
        "WHERE table_schema = DATABASE() AND table_name = '" + escaped_table + "' " +
        "ORDER BY ordinal_position";
    MysqlRows rows(conn, sql, error);
    if (!rows.ok()) {
        return false;
    }
    MYSQL_RES* res = rows.raw();
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr) {
        const std::string name = row[0] != nullptr ? row[0] : "";
        const std::string data_type = row[1] != nullptr ? row[1] : "";
        const std::string is_nullable = row[2] != nullptr ? row[2] : "";
        const std::string column_key = row[3] != nullptr ? row[3] : "";
        const std::string extra = row[4] != nullptr ? row[4] : "";

        // 生成列 / 隐藏列：排除在 ColumnInfo 之外（spec §八）。
        // 不进 Schema → SQL 生成器自动跳过，insert 全列 SQL 不会给生成列绑值。
        const std::string extra_upper = upper_copy(extra);
        if (extra_upper.find("VIRTUAL") != std::string::npos ||
            extra_upper.find("STORED") != std::string::npos ||
            extra_upper.find("INVISIBLE") != std::string::npos) {
            continue;
        }

        // 类型映射：不可表达的列 → 拒绝该数据源（对齐 SQLite BLOB 拒绝）。
        ValueKind kind = ValueKind::kText;
        if (!data_type_to_kind(data_type, &kind)) {
            *error = "unsupported column type \"" + table + "." + name + "\" of type \"" +
                     data_type + "\": the middleware only supports INTEGER/REAL/TEXT columns";
            return false;
        }

        ColumnInfo col;
        col.name = name;
        col.kind = kind;
        col.nullable = (is_nullable == "YES");
        col.primary_key = (column_key == "PRI");
        // Composite PK columns are not unique per column; unique is set
        // after the loop based on PK cardinality (single-column PK only).
        col.unique = false;
        // autoincrement：仅显式 AUTO_INCREMENT 才算（MySQL 无 SQLite 的 rowid 魔法）。
        col.autoincrement = extra_upper.find("AUTO_INCREMENT") != std::string::npos;
        out->columns.push_back(std::move(col));
    }
    // Single-column PK → that column is unique; composite PK → none per column.
    int pk_count = 0;
    std::size_t pk_index = 0;
    for (std::size_t i = 0; i < out->columns.size(); ++i) {
        if (out->columns[i].primary_key) {
            ++pk_count;
            pk_index = i;
        }
    }
    if (pk_count == 1) {
        out->columns[pk_index].unique = true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 唯一性自省（spec §五.2）
// ---------------------------------------------------------------------------
// 核心原则：只有「单列、非部分、整列强制」的唯一约束才标列级 unique。
// - 联合唯一（index 列数 > 1）：不标任何单列（对齐 SQLite D6）。
// - 前缀索引（sub_part 非空）：只强制前 N 字节，不标（对齐 SQLite 跳过 partial）。
// - 普通索引（non_unique=1）：忽略。
// 用 statistics 聚合判联合，绝不用 columns.column_key='UNI'（会误标联合唯一）。
bool introspect_unique_indexes(MYSQL* conn, TableInfo* table, std::string* error) {
    std::vector<char> esc_tbl(table->name.size() * 2 + 1);
    const unsigned long esc_len2 = mysql_real_escape_string(
        conn, esc_tbl.data(), table->name.c_str(),
        static_cast<unsigned long>(table->name.size()));
    const std::string escaped_tbl(esc_tbl.data(), esc_len2);
    const std::string sql =
        "SELECT index_name, column_name, sub_part, non_unique "
        "FROM information_schema.statistics "
        "WHERE table_schema = DATABASE() AND table_name = '" + escaped_tbl + "' " +
        "ORDER BY index_name, seq_in_index";
    MysqlRows rows(conn, sql, error);
    if (!rows.ok()) {
        return false;
    }
    // 聚合到 index_name：每个索引记 (列数, 是否唯一, 唯一列名, 是否有前缀)。
    struct IndexAgg {
        int column_count{0};
        bool is_unique{false};
        std::string sole_column;
        bool has_prefix{false};  // 任一列 sub_part 非空 → 前缀索引
    };
    std::unordered_map<std::string, IndexAgg> indexes;
    MYSQL_RES* res = rows.raw();
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr) {
        const std::string index_name = row[0] != nullptr ? row[0] : "";
        const std::string column_name = row[1] != nullptr ? row[1] : "";
        // sub_part: NULL 表示整列；非 NULL 字符串表示前缀长度。
        const bool has_prefix = row[2] != nullptr;
        const int non_unique = (row[3] != nullptr) ? std::atoi(row[3]) : 1;

        auto& agg = indexes[index_name];
        ++agg.column_count;
        agg.is_unique = (non_unique == 0);
        agg.sole_column = column_name;  // 单列索引时这就是唯一列
        if (has_prefix) {
            agg.has_prefix = true;
        }
    }
    // 只有「单列 + 唯一 + 整列（无前缀）」的索引才标列级 unique。
    // 跳过 PK 索引：PK 已在 introspect_columns 按 column_key='PRI' 标过。
    for (const auto& [index_name, agg] : indexes) {
        if (!agg.is_unique || agg.column_count != 1 || agg.has_prefix) {
            continue;
        }
        // PRIMARY 索引：跳过（已在列层标过）。
        if (upper_copy(index_name) == "PRIMARY") {
            continue;
        }
        for (auto& col : table->columns) {
            if (col.name == agg.sole_column) {
                col.unique = true;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 表清单 + 串起自省
// ---------------------------------------------------------------------------
bool introspect(MYSQL* conn, Schema* out, std::string* error) {
    // 只读 BASE TABLE，不读 view（中间件是行级 CRUD，不对视图做 insert/update/delete）。
    const std::string sql =
        "SELECT table_name FROM information_schema.tables "
        "WHERE table_schema = DATABASE() AND table_type = 'BASE TABLE' "
        "ORDER BY table_name";
    MysqlRows rows(conn, sql, error);
    if (!rows.ok()) {
        return false;
    }
    std::vector<std::string> names;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(rows.raw())) != nullptr) {
        if (row[0] != nullptr) {
            names.emplace_back(row[0]);
        }
    }
    // 表名已全部取到 names（mysql_store_result 把结果完整取到客户端，服务端已 flush）。
    // rows 在函数末尾析构释放结果集；列自省各自建独立的 MysqlRows，互不干扰。

    for (const auto& name : names) {
        TableInfo table;
        table.name = name;
        if (!introspect_columns(conn, name, &table, error)) {
            return false;
        }
        if (!introspect_unique_indexes(conn, &table, error)) {
            return false;
        }
        out->add_table(std::move(table));
    }
    return true;
}

// ---------------------------------------------------------------------------
// 标识符引用（对齐 sqlite_database 的 quote_ident，双保险）
// ---------------------------------------------------------------------------
// 拼进 SQL 的标识符都来自 schema 白名单（不是客户端原始输入），
// 仍然加反引号转义 —— 双保险，且表名含关键字时必需。
std::string quote_ident(const std::string& name) {
    std::string out;
    out.reserve(name.size() + 2);
    out += '`';
    for (const char c : name) {
        if (c == '`') {
            out += '`';  // 反引号转义：`` 表示一个字面反引号
        }
        out += c;
    }
    out += '`';
    return out;
}

// ---------------------------------------------------------------------------
// 错误码映射（spec §十，对齐 sqlite_database 的 map_constraint_error）
// ---------------------------------------------------------------------------
// MySQL 用 errno（驱动级）+ SQLSTATE。约束类映射到 400/409 分界：
//   errno 1062 ER_DUP_ENTRY         → kAlreadyExists（HTTP 409）
//   errno 1048 ER_BAD_NULL_ERROR    → kInvalidRequest（HTTP 400，客户端漏传必填列）
//   errno 3819/4092 CHECK 违例      → kInvalidRequest（HTTP 400，MySQL 8.0.16+）
// 其余 → kBackendError（HTTP 500）。
DbStatus map_mysql_error(unsigned int errno_val, std::string* message,
                         const std::string& driver_msg) {
    switch (errno_val) {
        case 1062:  // ER_DUP_ENTRY：唯一键 / PK 冲突
            *message = "unique constraint violation: " + driver_msg;
            return DbStatus::kAlreadyExists;
        case 1048:  // ER_BAD_NULL_ERROR：NOT NULL 违例
            *message = "constraint violation: " + driver_msg;
            return DbStatus::kInvalidRequest;
        case 3819:  // ER_CHECK_CONSTRAINT_VIOLATED（8.0.16+）
        case 4092:  // ER_CHECK_CONSTRAINT_VIOLATED（另一变体）
            *message = "constraint violation: " + driver_msg;
            return DbStatus::kInvalidRequest;
        default:
            *message = driver_msg;
            return DbStatus::kBackendError;
    }
}

// ---------------------------------------------------------------------------
// SQL 生成（对齐 sqlite_database 的模板策略：每表每操作每 where 列一条固定语句）
// ---------------------------------------------------------------------------
std::string select_sql(const TableInfo& table, const std::string& where_column,
                           bool lock_for_update = false) {
    std::string sql = "SELECT ";
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += quote_ident(table.columns[i].name);
    }
    sql += " FROM " + quote_ident(table.name) +
           " WHERE " + quote_ident(where_column) + " = ?";
    if (lock_for_update) {
        sql += " FOR UPDATE";
    }
    return sql;
}

std::string insert_sql(const TableInfo& table) {
    std::string cols;
    std::string marks;
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
        if (i > 0) {
            cols += ", ";
            marks += ", ";
        }
        cols += quote_ident(table.columns[i].name);
        marks += "?";
    }
    return "INSERT INTO " + quote_ident(table.name) + " (" + cols +
           ") VALUES (" + marks + ")";
}

std::string update_sql(const TableInfo& table, const std::string& where_column) {
    std::string sets;
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
        if (i > 0) {
            sets += ", ";
        }
        sets += quote_ident(table.columns[i].name) + " = ?";
    }
    return "UPDATE " + quote_ident(table.name) + " SET " + sets +
           " WHERE " + quote_ident(where_column) + " = ?";
}

std::string delete_sql(const TableInfo& table, const std::string& where_column) {
    return "DELETE FROM " + quote_ident(table.name) +
           " WHERE " + quote_ident(where_column) + " = ?";
}

}  // namespace

// ---------------------------------------------------------------------------
// MysqlStmt
// ---------------------------------------------------------------------------
// prepared statement 的 RAII 封装（对齐 SqliteStmt）。关键差异见头文件注释：
// - MYSQL_BIND.buffer 驱动不拷贝，绑定的 string 暂存 bind_strs_；
// - 连接级错误置 broken_，valid() 返回 prepared_ok_ && !broken_，触发重新 prepare。
MysqlStmt::MysqlStmt(MYSQL* conn, const std::string& sql) {
    stmt_ = mysql_stmt_init(conn);
    if (stmt_ == nullptr) {
        error_ = "mysql_stmt_init failed (out of memory)";
        return;
    }
    if (mysql_stmt_prepare(stmt_, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0) {
        error_ = std::string("mysql_stmt_prepare failed: ") + mysql_stmt_error(stmt_) +
                 " (sql: " + sql + ")";
        prepared_ok_ = false;
        return;
    }
    prepared_ok_ = true;
}

MysqlStmt::~MysqlStmt() {
    if (stmt_ != nullptr) {
        mysql_stmt_close(stmt_);
    }
}

bool MysqlStmt::rebind_reset() {
    if (stmt_ == nullptr) {
        return false;
    }
    // mysql_stmt_reset：清服务端当前状态（对齐 sqlite3_reset + clear_bindings）。
    if (mysql_stmt_reset(stmt_) != 0) {
        const unsigned int e = mysql_stmt_errno(stmt_);
        if (e == CR_SERVER_LOST || e == CR_SERVER_GONE_ERROR || e == CR_STMT_CLOSED) {
            broken_ = true;
        }
        error_ = mysql_stmt_error(stmt_);
        return false;
    }
    // 写缓冲作废，下次 bind_params 重新填。
    write_slots_.clear();
    write_binds_.clear();
    return true;
}

bool MysqlStmt::bind_params(const std::vector<Value>& values) {
    if (stmt_ == nullptr) {
        return false;
    }
    const unsigned int param_count = static_cast<unsigned int>(mysql_stmt_param_count(stmt_));
    if (static_cast<unsigned int>(values.size()) != param_count) {
        error_ = "param count mismatch: expected " + std::to_string(param_count) +
                 ", got " + std::to_string(values.size());
        return false;
    }
    write_slots_.clear();
    write_binds_.clear();
    write_slots_.resize(param_count);
    write_binds_.resize(param_count);
    for (unsigned int i = 0; i < param_count; ++i) {
        MYSQL_BIND& b = write_binds_[i];
        WriteSlot& s = write_slots_[i];
        std::memset(&b, 0, sizeof(MYSQL_BIND));
        const Value& v = values[i];
        if (std::holds_alternative<std::monostate>(v)) {
            s.is_null = true;
            b.buffer_type = MYSQL_TYPE_NULL;
            b.is_null = &s.is_null;
        } else if (std::holds_alternative<std::int64_t>(v)) {
            s.iv = static_cast<long long>(std::get<std::int64_t>(v));
            b.buffer_type = MYSQL_TYPE_LONGLONG;
            b.buffer = &s.iv;
            b.buffer_length = sizeof(long long);
            b.is_unsigned = 0;
            b.is_null = &s.is_null;
        } else if (std::holds_alternative<double>(v)) {
            s.dv = std::get<double>(v);
            b.buffer_type = MYSQL_TYPE_DOUBLE;
            b.buffer = &s.dv;
            b.buffer_length = sizeof(double);
            b.is_null = &s.is_null;
        } else {
            const std::string& sv = std::get<std::string>(v);
            s.sv = sv;  // 拷贝持有，生命周期覆盖 execute
            b.buffer_type = MYSQL_TYPE_STRING;
            b.buffer = const_cast<char*>(s.sv.data());
            b.buffer_length = static_cast<unsigned long>(s.sv.size());
            s.length = static_cast<unsigned long>(s.sv.size());
            b.length = &s.length;
            b.is_null = &s.is_null;
        }
    }
    if (mysql_stmt_bind_param(stmt_, write_binds_.data()) != 0) {
        const unsigned int e = mysql_stmt_errno(stmt_);
        if (e == CR_SERVER_LOST || e == CR_SERVER_GONE_ERROR || e == CR_STMT_CLOSED) {
            broken_ = true;
        }
        error_ = mysql_stmt_error(stmt_);
        return false;
    }
    return true;
}

RowCount MysqlStmt::execute_write(DbStatus* status, std::string* message) {
    if (stmt_ == nullptr) {
        *status = DbStatus::kBackendError;
        *message = error_.empty() ? "mysql statement not prepared" : error_;
        return 0;
    }
    if (mysql_stmt_execute(stmt_) != 0) {
        const unsigned int e = mysql_stmt_errno(stmt_);
        if (e == CR_SERVER_LOST || e == CR_SERVER_GONE_ERROR || e == CR_STMT_CLOSED) {
            broken_ = true;
        }
        *status = map_mysql_error(e, message, mysql_stmt_error(stmt_));
        return 0;
    }
    *status = DbStatus::kOk;
    *message = "";
    return static_cast<RowCount>(mysql_stmt_affected_rows(stmt_));
}

bool MysqlStmt::bind_result() {
    if (stmt_ == nullptr) {
        return false;
    }
    const unsigned int field_count = static_cast<unsigned int>(mysql_stmt_field_count(stmt_));
    if (field_count == 0) {
        return false;
    }
    MYSQL_RES* meta = mysql_stmt_result_metadata(stmt_);
    if (meta == nullptr) {
        return false;  // 无结果集元信息
    }
    read_slots_.clear();
    read_binds_.clear();
    read_slots_.resize(field_count);
    read_binds_.resize(field_count);
    for (unsigned int i = 0; i < field_count; ++i) {
        MYSQL_FIELD* f = mysql_fetch_field_direct(meta, i);
        MYSQL_BIND& b = read_binds_[i];
        ReadSlot& s = read_slots_[i];
        std::memset(&b, 0, sizeof(MYSQL_BIND));
        b.is_null = &s.is_null;
        b.error = &s.error;
        b.length = &s.length;
        switch (f->type) {
            case MYSQL_TYPE_TINY:
            case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_INT24:
            case MYSQL_TYPE_LONG:
            case MYSQL_TYPE_LONGLONG:
            case MYSQL_TYPE_YEAR:
                b.buffer_type = MYSQL_TYPE_LONGLONG;
                b.buffer = &s.iv;
                b.buffer_length = sizeof(long long);
                // Read UNSIGNED_FLAG to know if the column is unsigned.
                s.is_unsigned = (f->flags & UNSIGNED_FLAG) != 0;
                b.is_unsigned = s.is_unsigned;
                break;
            case MYSQL_TYPE_FLOAT:
            case MYSQL_TYPE_DOUBLE:
            case MYSQL_TYPE_DECIMAL:
            case MYSQL_TYPE_NEWDECIMAL:
                b.buffer_type = MYSQL_TYPE_DOUBLE;
                b.buffer = &s.dv;
                b.buffer_length = sizeof(double);
                break;
            default: {
                // 文本族（含 DATE/DATETIME/TIMESTAMP/TIME/ENUM/SET/CHAR/VARCHAR/TEXT）。
                // 预分配缓冲上限 kMaxTextFetchBytes（不用 f->length —— LONGTEXT 可达 4GB，
                // 按声明长度分配会 OOM）。超出上限的值驱动设 s.error=1（截断），
                // read_column 据此拒绝（中间件只支持单值 TEXT，超长=异构数据）。
                const unsigned long len = kMaxTextFetchBytes + 1;
                s.raw.assign(len, '\0');
                b.buffer_type = MYSQL_TYPE_STRING;
                b.buffer = s.raw.data();
                b.buffer_length = len;
                break;
            }
        }
    }
    mysql_free_result(meta);
    if (mysql_stmt_bind_result(stmt_, read_binds_.data()) != 0) {
        const unsigned int e = mysql_stmt_errno(stmt_);
        if (e == CR_SERVER_LOST || e == CR_SERVER_GONE_ERROR || e == CR_STMT_CLOSED) {
            broken_ = true;
        }
        error_ = mysql_stmt_error(stmt_);
        return false;
    }
    return true;
}

bool MysqlStmt::execute_read() {
    if (stmt_ == nullptr) {
        return false;
    }
    if (mysql_stmt_execute(stmt_) != 0) {
        const unsigned int e = mysql_stmt_errno(stmt_);
        if (e == CR_SERVER_LOST || e == CR_SERVER_GONE_ERROR || e == CR_STMT_CLOSED) {
            broken_ = true;
        }
        error_ = mysql_stmt_error(stmt_);
        return false;
    }
    // store_result：把完整结果集缓冲到客户端，并让驱动在 fetch 后回填每列的
    // error 标记（MYSQL_DATA_TRUNCATED → s.error=1）。read_column 据此拒绝被
    // 截断的异构数据（对齐 SQLite 读侧「foreign data of wrong type」契约）。
    // 未调 store_result 时 fetch 不回填 error 标记，截断会静默成部分数据。
    if (mysql_stmt_store_result(stmt_) != 0) {
        const unsigned int e = mysql_stmt_errno(stmt_);
        if (e == CR_SERVER_LOST || e == CR_SERVER_GONE_ERROR || e == CR_STMT_CLOSED) {
            broken_ = true;
        }
        error_ = mysql_stmt_error(stmt_);
        return false;
    }
    return true;
}

bool MysqlStmt::fetch_row() {
    if (stmt_ == nullptr) {
        return false;
    }
    const int rc = mysql_stmt_fetch(stmt_);
    if (rc == 0 || rc == MYSQL_DATA_TRUNCATED) {
        return true;
    }
    if (rc == MYSQL_NO_DATA) {
        return false;
    }
    const unsigned int e = mysql_stmt_errno(stmt_);
    if (e == CR_SERVER_LOST || e == CR_SERVER_GONE_ERROR || e == CR_STMT_CLOSED) {
        broken_ = true;
    }
    error_ = mysql_stmt_error(stmt_);
    return false;
}

bool MysqlStmt::read_column(std::size_t i, const TableInfo& table, Value* out) {
    if (i >= read_slots_.size()) {
        return false;
    }
    const ReadSlot& s = read_slots_[i];
    if (s.is_null) {
        *out = Value{};
        return true;
    }
    const ValueKind kind = table.columns[i].kind;
    if (kind == ValueKind::kInteger) {
        // Reject unsigned values that overflow int64_t.
        if (s.is_unsigned && static_cast<unsigned long long>(s.iv) >
                             static_cast<unsigned long long>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        *out = Value{static_cast<std::int64_t>(s.iv)};
        return true;
    }
    if (kind == ValueKind::kReal) {
        if (s.error) {
            return false;
        }
        *out = Value{s.dv};
        return true;
    }
    // TEXT：取实际长度（s.length），不依赖 NUL 终止。
    // store_result 后，驱动在 fetch 时回填 s.error=1 表示该列被截断
    // （实际数据超过预分配缓冲）。中间件只支持 TEXT，按字段声明长度预分配，
    // 合法值不会截断；截断即异构/超长数据，拒绝（对齐 SQLite 读侧规范化）。
    if (s.error) {
        return false;
    }
    const std::size_t n = static_cast<std::size_t>(s.length);
    if (n > read_slots_[i].raw.size()) {
        return false;
    }
    *out = Value{std::string(read_slots_[i].raw.data(), n)};
    return true;
}

// ---------------------------------------------------------------------------
// MysqlDatabase
// ---------------------------------------------------------------------------

MysqlDatabase::MysqlDatabase(MYSQL* conn, Schema schema, std::string name)
    : Database(std::move(schema), std::move(name)), conn_(conn) {}

MysqlDatabase::~MysqlDatabase() {
    cache_.clear();
    // 成员逆序：连接在最后销构。阶段 3 补 StatementCache 后，
    // cache_ 必须先于 conn_ 析构（对齐 SqliteDatabase 的析构顺序约定）。
    if (conn_ != nullptr) {
        mysql_close(conn_);
    }
}

bool MysqlDatabase::healthy() {
    if (conn_ == nullptr) {
        return false;
    }
    // mysql_ping：轻量探活，对齐 SQLite 的 SELECT 1。
    // 不依赖 MySQL 的自动重连（MYSQL_OPT_RECONNECT=false）：ping 返回非零
    // 即不健康，交连接池销毁重建（对齐 spec §十一 的 healthy 语义）。
    return mysql_ping(conn_) == 0;
}

// 阶段 3：四个 do_* + MysqlStmt prepared statement + map_mysql_error。
//
// SQL 生成策略对齐 SQLite（每表每操作每 where 列一条固定语句，经 cache_ 复用）。
// 读侧类型规范化对齐 SqliteDatabase::do_select：可无损提升的修正（REAL 列整数→double），
// 异构数据报 "foreign data of wrong type" 而非把 bad_variant_access 留给下游。

namespace {

// 投影：全列结果 → 请求的列子集（按请求顺序，空请求 = 全部列 schema 顺序）。
// 从 sqlite_database.cpp 搬为静态函数 —— 不改基类公开接口。
void project(const TableInfo& table, const std::vector<std::string>& wanted, ResultSet* rs) {
    if (wanted.empty()) {
        rs->columns.reserve(table.columns.size());
        for (const auto& col : table.columns) {
            rs->columns.push_back(col.name);
        }
        return;
    }
    std::vector<std::size_t> indexes;
    indexes.reserve(wanted.size());
    for (const auto& name : wanted) {
        for (std::size_t i = 0; i < table.columns.size(); ++i) {
            if (table.columns[i].name == name) {
                indexes.push_back(i);
                break;
            }
        }
    }
    rs->columns = wanted;
    for (auto& row : rs->rows) {
        std::vector<Value> picked;
        picked.reserve(indexes.size());
        for (const std::size_t i : indexes) {
            picked.push_back(row[i]);
        }
        row = std::move(picked);
    }
}

template <typename Pairs>
const Value* find_value(const Pairs& pairs, const std::string& column) {
    for (const auto& [name, value] : pairs) {
        if (name == column) {
            return &value;
        }
    }
    return nullptr;
}

bool has_autoincrement(const TableInfo& table) {
    for (const auto& col : table.columns) {
        if (col.autoincrement) {
            return true;
        }
    }
    return false;
}

}  // namespace

DbResult<ResultSet> MysqlDatabase::do_select(const SelectRequest& req) {
    const TableInfo* table = schema_.find_table(req.table);
    // 校验管道已保证 table 存在、where 列唯一；此处防御性判空。
    if (table == nullptr) {
        return {DbStatus::kBackendError, ResultSet{}, "unknown table in do_select"};
    }
    const std::string key = "SEL|" + req.table + "|" + req.where.column;
    std::unique_ptr<MysqlStmt>* holder = &cache_.get_or_create(key, [this, &table, &req] {
        return std::make_unique<MysqlStmt>(conn_, select_sql(*table, req.where.column));
    });
    MysqlStmt* stmt = holder->get();
    // broken_ 的缓存条目：丢弃旧条目重新构造（StatementCache 的 get_or_create 契约
    // 不支持替换，这里以新对象覆写 holder，下次仍按 key 命中覆写后的条目）。
    if (stmt != nullptr && !stmt->valid()) {
        *holder = std::make_unique<MysqlStmt>(conn_, select_sql(*table, req.where.column));
        stmt = holder->get();
    }
    if (stmt == nullptr || !stmt->valid()) {
        return {DbStatus::kBackendError, ResultSet{}, stmt != nullptr ? stmt->error() : "null stmt"};
    }
    if (!stmt->rebind_reset()) {
        return {DbStatus::kBackendError, ResultSet{}, stmt->error()};
    }
    std::vector<Value> params{req.where.value};
    if (!stmt->bind_params(params)) {
        return {DbStatus::kBackendError, ResultSet{}, stmt->error()};
    }
    // 读侧：bind_result 按列准备缓冲 → execute → fetch。
    if (!stmt->bind_result()) {
        return {DbStatus::kBackendError, ResultSet{}, stmt->error()};
    }
    if (!stmt->execute_read()) {
        return {DbStatus::kBackendError, ResultSet{}, stmt->error()};
    }
    ResultSet rs;
    if (stmt->fetch_row()) {
        std::vector<Value> row;
        row.reserve(table->columns.size());
        for (std::size_t i = 0; i < table->columns.size(); ++i) {
            Value v;
            if (!stmt->read_column(i, *table, &v)) {
                // 任何返回路径都必须 rebind_reset：store_result 缓冲的结果集
                // 若不消费完/reset，下次同 stmt execute 会报 "Commands out of sync"。
                (void)stmt->rebind_reset();
                return {DbStatus::kBackendError, ResultSet{},
                        "foreign data of wrong type in column \"" +
                            table->columns[i].name + "\""};
            }
            // 读侧规范化（对齐 SqliteDatabase）：REAL 列整数→double 提升；
            // 其余类型不符报错。MySQL 类型由自省映射，正常路径不会不符，
            // 但外部（CLI/其他工具）可绕过校验管道写入异构值。
            const ValueKind kind = table->columns[i].kind;
            if (kind == ValueKind::kReal && std::holds_alternative<std::int64_t>(v)) {
                v = Value{static_cast<double>(std::get<std::int64_t>(v))};
            } else if (!is_null(v) && kind_of(v) != kind) {
                (void)stmt->rebind_reset();
                return {DbStatus::kBackendError, ResultSet{},
                        "foreign data of wrong type in column \"" +
                            table->columns[i].name + "\""};
            }
            row.push_back(std::move(v));
        }
        rs.rows.push_back(std::move(row));
        // 消费到结束：where 列必唯一（校验管道保证）至多一行。
        (void)stmt->fetch_row();
    }
    // fetch 后必须 reset，否则下次同 stmt 的 execute 会报 "Commands out of sync"。
    (void)stmt->rebind_reset();
    project(*table, req.columns, &rs);
    return {DbStatus::kOk, std::move(rs), ""};
}

DbResult<InsertResult> MysqlDatabase::do_insert(const InsertRequest& req) {
    const TableInfo* table = schema_.find_table(req.table);
    if (table == nullptr) {
        return {DbStatus::kBackendError, InsertResult{}, "unknown table in do_insert"};
    }
    const std::string key = "INS|" + req.table;
    std::unique_ptr<MysqlStmt>* holder = &cache_.get_or_create(key, [this, &table] {
        return std::make_unique<MysqlStmt>(conn_, insert_sql(*table));
    });
    MysqlStmt* stmt = holder->get();
    if (stmt != nullptr && !stmt->valid()) {
        *holder = std::make_unique<MysqlStmt>(conn_, insert_sql(*table));
        stmt = holder->get();
    }
    if (stmt == nullptr || !stmt->valid()) {
        return {DbStatus::kBackendError, InsertResult{}, stmt != nullptr ? stmt->error() : "null stmt"};
    }
    if (!stmt->rebind_reset()) {
        return {DbStatus::kBackendError, InsertResult{}, stmt->error()};
    }
    // 全列固定 SQL（口径 D3）：缺省列绑 NULL。
    std::vector<Value> params;
    params.reserve(table->columns.size());
    for (const auto& col : table->columns) {
        const Value* given = find_value(req.values, col.name);
        params.push_back(given != nullptr ? *given : Value{});
    }
    if (!stmt->bind_params(params)) {
        return {DbStatus::kBackendError, InsertResult{}, stmt->error()};
    }
    DbStatus status;
    std::string message;
    const RowCount affected = stmt->execute_write(&status, &message);
    if (status != DbStatus::kOk) {
        return {status, InsertResult{}, std::move(message)};
    }
    InsertResult result;
    result.affected_rows = affected;
    if (has_autoincrement(*table)) {
        // mysql_stmt_insert_id 返回本连接本次的自增值；显式 PK 也被记录
        // （对齐 SQLite 的 last_insert_rowid 口径 D12）。
        const my_ulonglong id = mysql_stmt_insert_id(stmt->raw());
        if (id != 0) {
            result.inserted_id = static_cast<std::int64_t>(id);
        }
    }
    return {DbStatus::kOk, std::move(result), ""};
}

DbResult<RowCount> MysqlDatabase::do_update(const UpdateRequest& req) {
    const TableInfo* table = schema_.find_table(req.table);
    if (table == nullptr) {
        return {DbStatus::kBackendError, 0, "unknown table in do_update"};
    }
    // 读改写（口径 D2）+ 隐式事务（spec §七，阻塞项）：
    // MySQL MVCC 默认 REPEATABLE READ，读快照与写当前读是两个时间点视图，
    // 中间被并发事务改了会用旧值回填 → 丢失更新。把读改写收敛到事务内。
    if (mysql_query(conn_, "START TRANSACTION") != 0) {
        return {DbStatus::kBackendError, 0,
                std::string("start transaction failed: ") + mysql_error(conn_)};
    }
    // RAII：无论哪条路径返回，事务都收尾（COMMIT 或 ROLLBACK），不悬挂。
    struct TxGuard {
        MYSQL* c;
        bool finished{false};
        bool commit{false};
        ~TxGuard() {
            if (!finished) {
                // 任何异常 / 提前返回路径：ROLLBACK 兜底（0 行分支见下）。
                mysql_query(c, "ROLLBACK");
            } else if (commit) {
                mysql_query(c, "COMMIT");
            } else {
                mysql_query(c, "ROLLBACK");  // 0 行：显式回滚（决策②，状态干净）
            }
        }
    } guard{conn_};

    // 读当前行。
    SelectRequest current{req.table, {}, req.where};
    // Locking read: SELECT ... FOR UPDATE to prevent lost updates.
    const std::string lock_key = "UPDSEL|" + req.table + "|" + req.where.column;
    std::unique_ptr<MysqlStmt>* lock_holder = &cache_.get_or_create(lock_key, [this, &table, &req] {
        return std::make_unique<MysqlStmt>(conn_, select_sql(*table, req.where.column, true));
    });
    MysqlStmt* lock_stmt = lock_holder->get();
    if (lock_stmt != nullptr && !lock_stmt->valid()) {
        *lock_holder = std::make_unique<MysqlStmt>(conn_, select_sql(*table, req.where.column, true));
        lock_stmt = lock_holder->get();
    }
    if (lock_stmt == nullptr || !lock_stmt->valid()) {
        return {DbStatus::kBackendError, 0,
                lock_stmt != nullptr ? lock_stmt->error() : "null lock stmt"};
    }
    if (!lock_stmt->rebind_reset()) {
        return {DbStatus::kBackendError, 0, lock_stmt->error()};
    }
    if (!lock_stmt->bind_params({req.where.value})) {
        return {DbStatus::kBackendError, 0, lock_stmt->error()};
    }
    if (!lock_stmt->bind_result()) {
        return {DbStatus::kBackendError, 0, lock_stmt->error()};
    }
    if (!lock_stmt->execute_read()) {
        return {DbStatus::kBackendError, 0, lock_stmt->error()};
    }
    ResultSet lock_rs;
    lock_rs.columns.reserve(table->columns.size());
    for (const auto& column : table->columns) {
        lock_rs.columns.push_back(column.name);
    }
    if (table->columns.size() == 0) {
        // no columns? treat as empty
    }
    if (lock_stmt->fetch_row()) {
        std::vector<Value> lock_row;
        lock_row.reserve(table->columns.size());
        for (std::size_t i = 0; i < table->columns.size(); ++i) {
            Value v;
            if (!lock_stmt->read_column(i, *table, &v)) {
                return {DbStatus::kBackendError, 0, "foreign data of wrong type in locking read"};
            }
            lock_row.push_back(std::move(v));
        }
        lock_rs.rows.push_back(std::move(lock_row));
    }
    const DbResult<ResultSet> cur{DbStatus::kOk, std::move(lock_rs), ""};
    if (cur.status != DbStatus::kOk) {
        return {cur.status, 0, cur.message};  // guard ROLLBACK
    }
    if (cur.value.rows.empty()) {
        // 0 行：决策②，kOk + 0 行；事务显式收尾（不 COMMIT 未改数据）。
        guard.finished = true;
        return {DbStatus::kOk, 0, ""};
    }
    const std::vector<Value>& row = cur.value.rows[0];

    const std::string key = "UPD|" + req.table + "|" + req.where.column;
    std::unique_ptr<MysqlStmt>* holder = &cache_.get_or_create(key, [this, &table, &req] {
        return std::make_unique<MysqlStmt>(conn_, update_sql(*table, req.where.column));
    });
    MysqlStmt* stmt = holder->get();
    if (stmt != nullptr && !stmt->valid()) {
        *holder = std::make_unique<MysqlStmt>(conn_, update_sql(*table, req.where.column));
        stmt = holder->get();
    }
    if (stmt == nullptr || !stmt->valid()) {
        return {DbStatus::kBackendError, 0, stmt != nullptr ? stmt->error() : "null stmt"};
    }
    if (!stmt->rebind_reset()) {
        return {DbStatus::kBackendError, 0, stmt->error()};
    }
    // 全列 SET（缺省列以现值回填，固定 SQL 形态）+ where 值。
    std::vector<Value> params;
    params.reserve(table->columns.size() + 1);
    for (std::size_t i = 0; i < table->columns.size(); ++i) {
        const Value* given = find_value(req.set, table->columns[i].name);
        params.push_back(given != nullptr ? *given : row[i]);
    }
    params.push_back(req.where.value);
    if (!stmt->bind_params(params)) {
        return {DbStatus::kBackendError, 0, stmt->error()};
    }
    DbStatus status;
    std::string message;
    const RowCount affected = stmt->execute_write(&status, &message);
    if (status != DbStatus::kOk) {
        return {status, 0, std::move(message)};  // guard ROLLBACK
    }
    guard.finished = true;
    // Explicit COMMIT with error check (don't rely on guard destructor).
    if (mysql_query(conn_, "COMMIT") != 0) {
        guard.finished = true;
        mysql_query(conn_, "ROLLBACK");
        return {DbStatus::kBackendError, 0,
                std::string("commit failed: ") + mysql_error(conn_)};
    }
    guard.finished = true;
    return {DbStatus::kOk, affected, ""};
}

DbResult<RowCount> MysqlDatabase::do_remove(const DeleteRequest& req) {
    const TableInfo* table = schema_.find_table(req.table);
    if (table == nullptr) {
        return {DbStatus::kBackendError, 0, "unknown table in do_remove"};
    }
    const std::string key = "DEL|" + req.table + "|" + req.where.column;
    std::unique_ptr<MysqlStmt>* holder = &cache_.get_or_create(key, [this, &table, &req] {
        return std::make_unique<MysqlStmt>(conn_, delete_sql(*table, req.where.column));
    });
    MysqlStmt* stmt = holder->get();
    if (stmt != nullptr && !stmt->valid()) {
        *holder = std::make_unique<MysqlStmt>(conn_, delete_sql(*table, req.where.column));
        stmt = holder->get();
    }
    if (stmt == nullptr || !stmt->valid()) {
        return {DbStatus::kBackendError, 0, stmt != nullptr ? stmt->error() : "null stmt"};
    }
    if (!stmt->rebind_reset()) {
        return {DbStatus::kBackendError, 0, stmt->error()};
    }
    std::vector<Value> params{req.where.value};
    if (!stmt->bind_params(params)) {
        return {DbStatus::kBackendError, 0, stmt->error()};
    }
    DbStatus status;
    std::string message;
    const RowCount affected = stmt->execute_write(&status, &message);
    if (status != DbStatus::kOk) {
        return {status, 0, std::move(message)};
    }
    return {DbStatus::kOk, affected, ""};
}

// ---------------------------------------------------------------------------
// 工厂：make_mysql_database
// ---------------------------------------------------------------------------

DatabaseCreateResult make_mysql_database(const config::DataSourceConfig& cfg) {
    if (cfg.type != "mysql") {
        return fail_mysql("datasource \"" + cfg.name + "\": make_mysql_database called with type \"" +
                          cfg.type + "\"");
    }
    if (cfg.mysql_host.empty()) {
        return fail_mysql("datasource \"" + cfg.name + "\": mysql.host is required");
    }
    if (cfg.mysql_database.empty()) {
        return fail_mysql("datasource \"" + cfg.name + "\": mysql.database is required");
    }
    if (cfg.mysql_user.empty()) {
        return fail_mysql("datasource \"" + cfg.name + "\": mysql.user is required");
    }

    MYSQL* raw = mysql_init(nullptr);
    if (raw == nullptr) {
        // mysql_init 失败基本只可能是 OOM。
        return fail_mysql("datasource \"" + cfg.name + "\": mysql_init failed (out of memory)");
    }

    // 不自动重连：连接池层已有探活 + factory 重建（spec §十一）。
    // MySQL 自己再重连会与池的"重建失败保留空槽、下次 acquire 再试"自愈模型打架。
    const bool reconnect = false;
    mysql_options(raw, MYSQL_OPT_RECONNECT, &reconnect);
    mysql_options(raw, MYSQL_OPT_CONNECT_TIMEOUT, &kConnectTimeoutSec);
    mysql_options(raw, MYSQL_OPT_READ_TIMEOUT, &kReadTimeoutSec);
    mysql_options(raw, MYSQL_OPT_WRITE_TIMEOUT, &kWriteTimeoutSec);

    const unsigned int port = static_cast<unsigned int>(cfg.mysql_port);
    if (mysql_real_connect(raw, cfg.mysql_host.c_str(), cfg.mysql_user.c_str(),
                           cfg.mysql_password.c_str(), cfg.mysql_database.c_str(),
                           port, nullptr, 0) == nullptr) {
        // 连接失败：错误信息含主机/端口/库，便于排障；不含密码。
        std::string err = "datasource \"" + cfg.name + "\": cannot connect to mysql " +
                         cfg.mysql_user + "@" + cfg.mysql_host + ":" +
                         std::to_string(cfg.mysql_port) + "/" + cfg.mysql_database + ": " +
                         mysql_error(raw);
        mysql_close(raw);
        return fail_mysql(std::move(err));
    }

    // 字符集：中间件只支持 INTEGER/REAL/TEXT，utf8mb4 覆盖所有 TEXT 值。
    if (mysql_set_character_set(raw, "utf8mb4") != 0) {
        std::string err = "datasource \"" + cfg.name +
                          "\": cannot set charset utf8mb4: " + mysql_error(raw);
        mysql_close(raw);
        return fail_mysql(std::move(err));
    }

    // 阶段 2：自省填 Schema。失败则关连接返回 error。
    // 表名/列名不规范化（对齐 SQLite 口径，避免跨平台大小写漂移，见 Schema::find_table 注释）。
    Schema schema;
    std::string intro_error;
    if (!introspect(raw, &schema, &intro_error)) {
        mysql_close(raw);
        return fail_mysql("datasource \"" + cfg.name + "\": " + intro_error);
    }
    return {std::make_unique<MysqlDatabase>(raw, std::move(schema), cfg.name), ""};
}

void register_mysql_backend() {
    register_database_type("mysql", &make_mysql_database);
}

}  // namespace cpppools::db

#endif  // CPPPOOLS_USE_MYSQL
