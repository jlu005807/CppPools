// SQLite 后端：Database NVI 子类的第一个真实实现。
//
// 结构（对齐 spec §6/§10 与期3计划决策 D1-D14）：
// - 工厂一步完成：打开（READWRITE 不带 CREATE）→ 文件库 WAL/busy_timeout
//   → schema 自省 → 构造 SqliteDatabase
// - SQL 由有限模板生成：每表每操作每 where 列一条固定语句（键空间被
//   schema 限定），经 StatementCache 复用 prepared statement
// - update 采用读改写：SET 全部列，缺省列以当前值回填（固定 SQL 的代价）
// - 错误码映射：UNIQUE/PRIMARYKEY 冲突 → kAlreadyExists，其余 → kBackendError

#include "sqlite_database.h"

#include "db/statement_cache.h"

#include "cpppools/db/schema.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cpppools::db {

namespace {

// SQLite 锁等待上限。与连接池的 acquire 超时是两层语义：
// 这里是“库级写锁排队”，池那边是“借连接的等待预算”。
constexpr int kBusyTimeoutMs = 3000;

// 标识符引用：虽然拼进 SQL 的标识符都来自 schema 白名单（不是客户端
// 原始输入），仍然加引号转义 —— 双保险，且表名含空格/关键字时必需。
std::string quote_ident(const std::string& name) {
    std::string out;
    out.reserve(name.size() + 2);
    out += '"';
    for (const char c : name) {
        if (c == '"') {
            out += '"';
        }
        out += c;
    }
    out += '"';
    return out;
}

std::string upper_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

// 声明类型 → 逻辑类型，按 SQLite 亲和规则
// （https://www.sqlite.org/datatype3.html §3.1，判定顺序即规则顺序）。
// BLOB 亲和（含无声明）返回 false —— Value 无法表达，数据源拒绝创建。
// NUMERIC 亲和映射为 kReal（整数按校验管道的放行规则可写入）。
bool affinity_to_kind(const std::string& declared, ValueKind* out) {
    const std::string u = upper_copy(declared);
    if (u.find("INT") != std::string::npos) {
        *out = ValueKind::kInteger;
        return true;
    }
    if (u.find("CHAR") != std::string::npos || u.find("CLOB") != std::string::npos ||
        u.find("TEXT") != std::string::npos) {
        *out = ValueKind::kText;
        return true;
    }
    if (u.empty() || u.find("BLOB") != std::string::npos) {
        return false;
    }
    if (u.find("REAL") != std::string::npos || u.find("FLOA") != std::string::npos ||
        u.find("DOUB") != std::string::npos) {
        *out = ValueKind::kReal;
        return true;
    }
    *out = ValueKind::kReal;  // NUMERIC 亲和
    return true;
}

// prepared statement 的 RAII 封装。prepare 失败时 valid()=false、
// error() 携带 sqlite3_errmsg —— 缓存照常存放（坏句柄不会被执行）。
class SqliteStmt {
public:
    SqliteStmt() = default;
    SqliteStmt(sqlite3* db, const std::string& sql) {
        if (sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.size()),
                               &stmt_, nullptr) != SQLITE_OK) {
            error_ = sqlite3_errmsg(db);
        }
    }
    ~SqliteStmt() {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
    }
    SqliteStmt(const SqliteStmt&) = delete;
    SqliteStmt& operator=(const SqliteStmt&) = delete;
    SqliteStmt(SqliteStmt&&) = delete;
    SqliteStmt& operator=(SqliteStmt&&) = delete;

    bool valid() const { return stmt_ != nullptr; }
    const std::string& error() const { return error_; }
    sqlite3_stmt* raw() { return stmt_; }

    int step() { return sqlite3_step(stmt_); }

    // 复用前必须复位：清掉上一次的绑定与游标状态，
    // 否则 statement cache 下会出现参数串台。
    void rebind_reset() {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }

    int bind_value(int index, const Value& value) {
        struct Binder {
            SqliteStmt* self;
            int index;
            int operator()(std::monostate) const {
                return sqlite3_bind_null(self->stmt_, index);
            }
            int operator()(std::int64_t v) const {
                return sqlite3_bind_int64(self->stmt_, index, static_cast<sqlite3_int64>(v));
            }
            int operator()(double v) const {
                return sqlite3_bind_double(self->stmt_, index, v);
            }
            int operator()(const std::string& v) const {
                return sqlite3_bind_text(self->stmt_, index, v.c_str(),
                                         static_cast<int>(v.size()), SQLITE_TRANSIENT);
            }
        };
        return std::visit(Binder{this, index}, value);
    }

private:
    sqlite3_stmt* stmt_{nullptr};
    std::string error_;
};

// 读一列的值。SQLITE_BLOB 返回 false（自省已拒绝 BLOB 列，此处是防御）。
bool try_read_column(sqlite3_stmt* raw, int i, Value* out) {
    switch (sqlite3_column_type(raw, i)) {
        case SQLITE_INTEGER:
            *out = Value{static_cast<std::int64_t>(sqlite3_column_int64(raw, i))};
            return true;
        case SQLITE_FLOAT:
            *out = Value{sqlite3_column_double(raw, i)};
            return true;
        case SQLITE_TEXT: {
            const unsigned char* text = sqlite3_column_text(raw, i);
            const int len = sqlite3_column_bytes(raw, i);
            *out = Value{std::string(reinterpret_cast<const char*>(text),
                                     static_cast<std::size_t>(len))};
            return true;
        }
        case SQLITE_NULL:
            *out = Value{};
            return true;
        default:
            return false;
    }
}

DbStatus map_constraint_error(sqlite3* db, std::string* message) {
    const int ext = sqlite3_extended_errcode(db);
    if (ext == SQLITE_CONSTRAINT_UNIQUE || ext == SQLITE_CONSTRAINT_PRIMARYKEY) {
        *message = std::string("unique constraint violation: ") + sqlite3_errmsg(db);
        return DbStatus::kAlreadyExists;
    }
    // NOT NULL / CHECK 违例是客户端契约违反（漏传必填列等），不是后端故障：
    // 映射 kInvalidRequest → HTTP 400（spec §7.3 的 400/500 分界）。
    // 校验管道只查客户端提供的列，缺省的 NOT NULL 列（无 DEFAULT）会在此暴露。
    if (ext == SQLITE_CONSTRAINT_NOTNULL || ext == SQLITE_CONSTRAINT_CHECK) {
        *message = std::string("constraint violation: ") + sqlite3_errmsg(db);
        return DbStatus::kInvalidRequest;
    }
    *message = sqlite3_errmsg(db);
    return DbStatus::kBackendError;
}

// ---------------------------------------------------------------------------
// schema 自省
// ---------------------------------------------------------------------------

// PRAGMA table_info：列、类型、notnull、PK。
bool introspect_columns(sqlite3* db, const std::string& table, TableInfo* out,
                        std::string* error) {
    const std::string sql = "PRAGMA table_info(" + quote_ident(table) + ")";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.size()),
                           &stmt, nullptr) != SQLITE_OK) {
        *error = "introspection failed for table \"" + table + "\": " + sqlite3_errmsg(db);
        return false;
    }
    int pk_count = 0;
    std::size_t pk_index = 0;
    std::string pk_declared;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* type_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const std::string name = name_raw != nullptr ? name_raw : "";
        const std::string declared = type_raw != nullptr ? type_raw : "";
        const int notnull = sqlite3_column_int(stmt, 3);
        const int pk = sqlite3_column_int(stmt, 5);

        ValueKind kind = ValueKind::kText;
        if (!affinity_to_kind(declared, &kind)) {
            sqlite3_finalize(stmt);
            *error = "unsupported BLOB-affinity column \"" + table + "." + name +
                     "\": the middleware only supports INTEGER/REAL/TEXT columns";
            return false;
        }
        ColumnInfo col;
        col.name = name;
        col.kind = kind;
        // PK 列按定义 NOT NULL（即使 PRAGMA 对 rowid 别名报 notnull=0）。
        col.nullable = (notnull == 0) && (pk == 0);
        col.primary_key = pk > 0;
        col.unique = pk > 0;
        col.autoincrement = false;
        out->columns.push_back(std::move(col));
        if (pk > 0) {
            ++pk_count;
            pk_index = out->columns.size() - 1;
            pk_declared = declared;
        }
    }
    sqlite3_finalize(stmt);

    // rowid 别名判定：仅当声明**恰好**是 INTEGER 的独列 PK。
    // 口径 D5：记 autoincrement=true 且 nullable=false ——
    // 显式 NULL 会被 SQLite 静默换成 rowid，属于惊喜行为，直接拒绝。
    // 已知边界：WITHOUT ROWID 表的独列 INTEGER PK 会被同规则误判为别名，
    // inserted_id 可能给出无意义值 —— 中间件不支持 WITHOUT ROWID 表。
    if (pk_count == 1 && upper_copy(pk_declared) == "INTEGER") {
        out->columns[pk_index].autoincrement = true;
        out->columns[pk_index].nullable = false;
    }
    return true;
}

// PRAGMA index_list + index_info：单列 UNIQUE 索引 → 该列 unique。
// 跳过 origin='pk'（PK 已在列层标记）；跳过 **partial 索引**：
// 部分唯一索引只在谓词行子集上强制唯一，两行谓词外可合法同值，
// 误标会击穿「每操作至多一行」的安全不变量。
// 联合唯一不使任何单列唯一（口径 D6）。
bool introspect_unique_indexes(sqlite3* db, TableInfo* table, std::string* error) {
    const std::string list_sql = "PRAGMA index_list(" + quote_ident(table->name) + ")";
    sqlite3_stmt* list = nullptr;
    if (sqlite3_prepare_v2(db, list_sql.c_str(), static_cast<int>(list_sql.size()),
                           &list, nullptr) != SQLITE_OK) {
        *error = "introspection failed for indexes of \"" + table->name + "\": " +
                 sqlite3_errmsg(db);
        return false;
    }
    while (sqlite3_step(list) == SQLITE_ROW) {
        const int is_unique = sqlite3_column_int(list, 2);
        const char* origin_raw = reinterpret_cast<const char*>(sqlite3_column_text(list, 3));
        const std::string origin = origin_raw != nullptr ? origin_raw : "";
        const char* idx_name_raw = reinterpret_cast<const char*>(sqlite3_column_text(list, 1));
        const std::string idx_name = idx_name_raw != nullptr ? idx_name_raw : "";
        // index_list 第 4 列 partial（SQLite >= 3.8.0）。
        const int is_partial = sqlite3_column_int(list, 4);
        if (is_unique == 0 || origin == "pk" || idx_name.empty() || is_partial != 0) {
            continue;
        }
        const std::string info_sql = "PRAGMA index_info(" + quote_ident(idx_name) + ")";
        sqlite3_stmt* info = nullptr;
        if (sqlite3_prepare_v2(db, info_sql.c_str(), static_cast<int>(info_sql.size()),
                               &info, nullptr) != SQLITE_OK) {
            sqlite3_finalize(list);
            *error = "introspection failed for index \"" + idx_name + "\": " +
                     sqlite3_errmsg(db);
            return false;
        }
        std::string sole_column;
        int column_count = 0;
        while (sqlite3_step(info) == SQLITE_ROW) {
            ++column_count;
            const char* col_raw = reinterpret_cast<const char*>(sqlite3_column_text(info, 2));
            if (col_raw != nullptr) {
                sole_column = col_raw;
            }
        }
        sqlite3_finalize(info);
        if (column_count == 1) {
            for (auto& col : table->columns) {
                if (col.name == sole_column) {
                    col.unique = true;
                }
            }
        }
    }
    sqlite3_finalize(list);
    return true;
}

bool introspect(sqlite3* db, Schema* out, std::string* error) {
    static const char* kTablesSql =
        "SELECT name FROM sqlite_master "
        "WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kTablesSql, -1, &stmt, nullptr) != SQLITE_OK) {
        *error = std::string("schema introspection failed: ") + sqlite3_errmsg(db);
        return false;
    }
    std::vector<std::string> names;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (raw != nullptr) {
            names.emplace_back(raw);
        }
    }
    sqlite3_finalize(stmt);

    for (const auto& name : names) {
        TableInfo table;
        table.name = name;
        if (!introspect_columns(db, name, &table, error)) {
            return false;
        }
        if (!introspect_unique_indexes(db, &table, error)) {
            return false;
        }
        out->add_table(std::move(table));
    }
    return true;
}

// ---------------------------------------------------------------------------
// SqliteDatabase
// ---------------------------------------------------------------------------

class SqliteDatabase : public Database {
public:
    SqliteDatabase(sqlite3* db, Schema schema, std::string name)
        : Database(std::move(schema), std::move(name)), db_(db) {}

    ~SqliteDatabase() override {
        cache_.clear();
        // cache_ 先于 db_ 析构（成员逆序）：所有 statement 先 finalize，
        // 再关连接 —— sqlite3_close 不会遇到未释放的句柄。
        if (db_ != nullptr) {
            sqlite3_close(db_);
        }
    }

    bool healthy() override {
        SqliteStmt* stmt = stmt_for("HEALTH", "SELECT 1");
        if (!stmt->valid()) {
            return false;
        }
        stmt->rebind_reset();
        return stmt->step() == SQLITE_ROW;
    }

    std::string backend_type() const override { return "sqlite"; }

protected:
    DbResult<ResultSet> do_select(const SelectRequest& req) override {
        const TableInfo* table = schema_.find_table(req.table);
        SqliteStmt* stmt = select_stmt(*table, req.where.column);
        if (!stmt->valid()) {
            return {DbStatus::kBackendError, ResultSet{}, stmt->error()};
        }
        stmt->rebind_reset();
        if (stmt->bind_value(1, req.where.value) != SQLITE_OK) {
            return {DbStatus::kBackendError, ResultSet{}, sqlite3_errmsg(db_)};
        }

        ResultSet rs;
        const int rc = stmt->step();
        if (rc == SQLITE_ROW) {
            std::vector<Value> row;
            row.reserve(table->columns.size());
            for (std::size_t i = 0; i < table->columns.size(); ++i) {
                Value v;
                if (!try_read_column(stmt->raw(), static_cast<int>(i), &v)) {
                    return {DbStatus::kBackendError, ResultSet{},
                            "unexpected BLOB value in column \"" +
                                table->columns[i].name + "\""};
                }
                // 读侧类型规范化：SQLite 是动态类型，外部写入（CLI/其他工具）
                // 可绕过校验管道存入异构值。按 schema 口径收口：
                // 可无损修正的提升（NUMERIC 列的整数→double），否则报错
                // —— 而不是把 bad_variant_access 留给下游变成无解释的 500。
                const ValueKind kind = table->columns[i].kind;
                if (kind == ValueKind::kReal && std::holds_alternative<std::int64_t>(v)) {
                    v = Value{static_cast<double>(std::get<std::int64_t>(v))};
                } else if (!is_null(v) && kind_of(v) != kind) {
                    return {DbStatus::kBackendError, ResultSet{},
                            "foreign data of wrong type in column \"" +
                                table->columns[i].name + "\""};
                }
                row.push_back(std::move(v));
            }
            rs.rows.push_back(std::move(row));
            // 消费到 DONE：where 列必唯一（校验管道保证）至多一行，
            // 把语句状态走完更干净，也防御未来跨调用持有 stmt 的路径。
            (void)stmt->step();
        } else if (rc != SQLITE_DONE) {
            return {DbStatus::kBackendError, ResultSet{}, sqlite3_errmsg(db_)};
        }
        project(*table, req.columns, &rs);
        return {DbStatus::kOk, std::move(rs), ""};
    }

    DbResult<InsertResult> do_insert(const InsertRequest& req) override {
        const TableInfo* table = schema_.find_table(req.table);
        const std::string key = "INS|" + req.table;
        SqliteStmt* stmt = stmt_for(key, insert_sql(*table));
        if (!stmt->valid()) {
            return {DbStatus::kBackendError, InsertResult{}, stmt->error()};
        }
        stmt->rebind_reset();
        // 全列固定 SQL（口径 D3）：缺省列绑定 NULL。
        int index = 1;
        for (const auto& col : table->columns) {
            const Value* given = find_value(req.values, col.name);
            const int rc = (given != nullptr) ? stmt->bind_value(index, *given)
                                              : stmt->bind_value(index, Value{});
            if (rc != SQLITE_OK) {
                return {DbStatus::kBackendError, InsertResult{}, sqlite3_errmsg(db_)};
            }
            ++index;
        }
        const int rc = stmt->step();
        if (rc != SQLITE_DONE) {
            std::string message;
            const DbStatus status = map_constraint_error(db_, &message);
            return {status, InsertResult{}, std::move(message)};
        }
        InsertResult result;
        result.affected_rows = sqlite3_changes(db_);
        if (has_rowid_alias(*table)) {
            result.inserted_id = static_cast<std::int64_t>(sqlite3_last_insert_rowid(db_));
        }
        return {DbStatus::kOk, std::move(result), ""};
    }

    DbResult<RowCount> do_update(const UpdateRequest& req) override {
        const TableInfo* table = schema_.find_table(req.table);

        // 读改写（口径 D2）：先取当前行，缺省列以现值回填，
        // 使 UPDATE 语句保持“每表每 where 列一条”的固定形态。
        SelectRequest current{req.table, {}, req.where};
        const DbResult<ResultSet> cur = do_select(current);
        if (cur.status != DbStatus::kOk) {
            return {cur.status, 0, cur.message};
        }
        if (cur.value.rows.empty()) {
            return {DbStatus::kOk, 0, ""};  // 决策②：0 行影响不是错误
        }
        const std::vector<Value>& row = cur.value.rows[0];

        const std::string key = "UPD|" + req.table + "|" + req.where.column;
        SqliteStmt* stmt = stmt_for(key, update_sql(*table, req.where.column));
        if (!stmt->valid()) {
            return {DbStatus::kBackendError, 0, stmt->error()};
        }
        stmt->rebind_reset();
        int index = 1;
        for (std::size_t i = 0; i < table->columns.size(); ++i) {
            const Value* given = find_value(req.set, table->columns[i].name);
            const Value& v = (given != nullptr) ? *given : row[i];
            if (stmt->bind_value(index, v) != SQLITE_OK) {
                return {DbStatus::kBackendError, 0, sqlite3_errmsg(db_)};
            }
            ++index;
        }
        if (stmt->bind_value(index, req.where.value) != SQLITE_OK) {
            return {DbStatus::kBackendError, 0, sqlite3_errmsg(db_)};
        }
        const int rc = stmt->step();
        if (rc != SQLITE_DONE) {
            std::string message;
            const DbStatus status = map_constraint_error(db_, &message);
            return {status, 0, std::move(message)};
        }
        return {DbStatus::kOk, static_cast<RowCount>(sqlite3_changes(db_)), ""};
    }

    DbResult<RowCount> do_remove(const DeleteRequest& req) override {
        const TableInfo* table = schema_.find_table(req.table);
        const std::string key = "DEL|" + req.table + "|" + req.where.column;
        const std::string sql = "DELETE FROM " + quote_ident(table->name) +
                                " WHERE " + quote_ident(req.where.column) + " = ?";
        SqliteStmt* stmt = stmt_for(key, sql);
        if (!stmt->valid()) {
            return {DbStatus::kBackendError, 0, stmt->error()};
        }
        stmt->rebind_reset();
        if (stmt->bind_value(1, req.where.value) != SQLITE_OK) {
            return {DbStatus::kBackendError, 0, sqlite3_errmsg(db_)};
        }
        const int rc = stmt->step();
        if (rc != SQLITE_DONE) {
            std::string message;
            const DbStatus status = map_constraint_error(db_, &message);
            return {status, 0, std::move(message)};
        }
        return {DbStatus::kOk, static_cast<RowCount>(sqlite3_changes(db_)), ""};
    }

private:
    SqliteStmt* stmt_for(const std::string& key, const std::string& sql) {
        std::unique_ptr<SqliteStmt>& holder = cache_.get_or_create(key, [this, &sql] {
            return std::make_unique<SqliteStmt>(db_, sql);
        });
        // Recover from a cached invalid statement (e.g. schema changed after prepare).
        if (holder != nullptr && !holder->valid()) {
            holder = std::make_unique<SqliteStmt>(db_, sql);
        }
        return holder.get();
    }

    SqliteStmt* select_stmt(const TableInfo& table, const std::string& where_column) {
        const std::string key = "SEL|" + table.name + "|" + where_column;
        std::string sql = "SELECT ";
        for (std::size_t i = 0; i < table.columns.size(); ++i) {
            if (i > 0) {
                sql += ", ";
            }
            sql += quote_ident(table.columns[i].name);
        }
        sql += " FROM " + quote_ident(table.name) +
               " WHERE " + quote_ident(where_column) + " = ?";
        return stmt_for(key, sql);
    }

    static std::string insert_sql(const TableInfo& table) {
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

    static std::string update_sql(const TableInfo& table, const std::string& where_column) {
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

    // 投影：全列结果 → 请求的列子集（按请求顺序，口径 D4）。
    // 空请求 = 全部列（schema 顺序）。
    static void project(const TableInfo& table, const std::vector<std::string>& wanted,
                        ResultSet* rs) {
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
    static const Value* find_value(const Pairs& pairs, const std::string& column) {
        for (const auto& [name, value] : pairs) {
            if (name == column) {
                return &value;
            }
        }
        return nullptr;
    }

    static bool has_rowid_alias(const TableInfo& table) {
        for (const auto& col : table.columns) {
            if (col.autoincrement) {
                return true;
            }
        }
        return false;
    }

    sqlite3* db_{nullptr};
    StatementCache<std::unique_ptr<SqliteStmt>> cache_{64};
};

}  // namespace

DatabaseCreateResult make_sqlite_database(const config::DataSourceConfig& cfg) {
    if (cfg.sqlite_path.empty()) {
        return {nullptr, "datasource \"" + cfg.name + "\" of type sqlite has no sqlite_path"};
    }
    int flags = SQLITE_OPEN_READWRITE;  // 不带 CREATE：中间件不建库
    if (cfg.sqlite_path.rfind("file:", 0) == 0) {
        flags |= SQLITE_OPEN_URI;
    }
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(cfg.sqlite_path.c_str(), &raw, flags, nullptr) != SQLITE_OK) {
        std::string message = (raw != nullptr) ? sqlite3_errmsg(raw) : "out of memory";
        if (raw != nullptr) {
            sqlite3_close(raw);
        }
        return {nullptr, "cannot open sqlite database \"" + cfg.sqlite_path +
                             "\": " + message};
    }
    // WAL + busy_timeout 仅对文件库（口径 D10）：
    // :memory:/共享内存库的 filename 为空，且 WAL 不支持内存库。
    const char* filename = sqlite3_db_filename(raw, "main");
    if (filename != nullptr && *filename != '\0') {
        sqlite3_exec(raw, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    }
    const std::string busy = "PRAGMA busy_timeout=" + std::to_string(kBusyTimeoutMs);
    sqlite3_exec(raw, busy.c_str(), nullptr, nullptr, nullptr);

    Schema schema;
    std::string error;
    if (!introspect(raw, &schema, &error)) {
        sqlite3_close(raw);
        return {nullptr, "datasource \"" + cfg.name + "\": " + error};
    }
    return {std::make_unique<SqliteDatabase>(raw, std::move(schema), cfg.name), ""};
}

void register_sqlite_backend() {
    register_database_type("sqlite", &make_sqlite_database);
}

}  // namespace cpppools::db
