#pragma once

#include "cpppools/db/database.h"
#include "db/statement_cache.h"

#ifdef CPPPOOLS_USE_MYSQL
#include <mysql.h>

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace cpppools::db {

// Private test/implementation seam for running the production introspection path
// on a caller-owned session (for example, with a session-specific sql_mode).
bool mysql_introspect_schema(MYSQL* conn, Schema* out, std::string* error);

// MySQL prepared statement 的 RAII 封装（对齐 SqliteStmt 的对外形态）。
//
// 与 SQLite 的差异（spec §九 / §十二）：
// - MySQL 的 MYSQL_BIND.buffer 是指针，驱动**不拷贝**，必须存活到
//   mysql_stmt_execute 之后 —— 绑定的 string 暂存在本对象的 write_slots_ 里。
// - 连接被池重建后，旧 MYSQL_STMT 失效且不自动修复：broken_ 标记连接级
//   错误（CR_SERVER_LOST / CR_SERVER_GONE_ERROR / CR_STMT_CLOSED 等），
//   valid() 返回 prepared_ok_ && !broken_，触发 StatementCache 重新 prepare。
class MysqlStmt {
public:
    MysqlStmt() = default;
    MysqlStmt(MYSQL* conn, const std::string& sql);
    ~MysqlStmt();

    MysqlStmt(const MysqlStmt&) = delete;
    MysqlStmt& operator=(const MysqlStmt&) = delete;
    MysqlStmt(MysqlStmt&&) = delete;
    MysqlStmt& operator=(MysqlStmt&&) = delete;

    bool valid() const { return prepared_ok_ && !broken_; }
    const std::string& error() const { return error_; }
    MYSQL_STMT* raw() { return stmt_; }

    // ---- 写侧（insert/update/delete/select 的 WHERE 参数）----
    // 复用前必须复位：清掉上一次的绑定与游标状态（对齐 sqlite3_reset +
    // clear_bindings）。连接级错误置 broken_。复位后写缓冲失效，必须重新
    // bind 完全部参数再 execute。
    bool rebind_reset();

    // 攒齐全部参数一次性 bind_param（mysql_stmt_bind_param 是全部参数一次调用，
    // 与 SQLite 逐个 bind 不同）。按 schema 列顺序 bind_values[i] → 参数 i+1。
    // 绑定的 string 拷贝进 slots_ 保证生命周期覆盖 execute。
    bool bind_params(const std::vector<Value>& values);

    // 执行写语句（insert/update/delete）。返回 mysql_stmt_affected_rows。
    // 失败时按 errno 映射 status + 填 message（连接级错误置 broken_）。
    RowCount execute_write(DbStatus* status, std::string* message);

    // ---- 读侧（select 结果）----
    // 按结果列数准备 fetch 缓冲。一次 bind_result 后可多次 fetch。
    bool bind_result();

    // 执行 select（execute + store_result）。返回 true=执行成功（可 fetch）；
    // false=错误（置 broken_）。
    // store_result：缓冲完整结果集到客户端并让驱动回填每列的截断错误标记
    // （MYSQL_DATA_TRUNCATED → s.error），read_column 据此拒绝截断的异构数据。
    bool execute_read();

    // fetch 一行到 read_bufs_。返回 true=有行；false=无行或错误（错误置 broken_）。
    bool fetch_row();

    // 取第 i 列读到的值（fetch 成功后调用）。返回 false=类型不可表达（防御，
    // 自省已挡 BLOB/JSON，此处不该走到）。
    bool read_column(std::size_t i, const TableInfo& table, Value* out);

private:
    // MariaDB 10.x exposes MYSQL_BIND::is_null/error as my_bool* while
    // newer MySQL headers use bool*. Derive the pointee type from the
    // client header so the same source builds against either ABI.
    using MysqlBindBool = std::remove_pointer_t<
        decltype(std::declval<MYSQL_BIND>().is_null)>;

    // 写侧参数槽：每个参数一个槽，MYSQL_BIND.buffer 指向其中对应成员。
    // 用 vector + reserve 保证地址稳定（不 realloc）。
    struct WriteSlot {
        long long iv{0};
        double dv{0.0};
        std::string sv;
        unsigned long length{0};
        MysqlBindBool is_null{false};
    };
    std::vector<WriteSlot> write_slots_;
    std::vector<MYSQL_BIND> write_binds_;

    // 读侧列槽：每列一个，fetch 缓冲存活于此。
    struct ReadSlot {
        long long iv{0};
        double dv{0.0};
        std::vector<char> raw;   // text buffer, pre-allocated per column max length
        unsigned long length{0};
        MysqlBindBool is_null{false};
        MysqlBindBool error{false};
        bool is_unsigned{false};
    };
    std::vector<ReadSlot> read_slots_;
    std::vector<MYSQL_BIND> read_binds_;

    MYSQL_STMT* stmt_{nullptr};
    bool prepared_ok_{false};
    bool broken_{false};
    std::string error_;
};

// MySQL 后端：Database NVI 子类。
//
// 阶段 1：连接 + utf8mb4 + 探活（healthy）。
// 阶段 2：schema 自省（见 mysql_database.cpp 的自省口径）。
// 阶段 3：四个 do_* + MysqlStmt prepared statement + map_mysql_error。
class MysqlDatabase : public Database {
public:
    // 由 make_mysql_database 在工厂中调用：连接已建立（所有权移交），
    // schema 由自省填充。
    MysqlDatabase(MYSQL* conn, Schema schema, std::string name);
    ~MysqlDatabase() override;

    bool healthy() override;
    std::string backend_type() const override { return "mysql"; }

protected:
    DbResult<ResultSet> do_select(const SelectRequest&) override;
    DbResult<InsertResult> do_insert(const InsertRequest&) override;
    DbResult<RowCount> do_update(const UpdateRequest&) override;
    DbResult<RowCount> do_remove(const DeleteRequest&) override;

private:
    MYSQL* conn_{nullptr};
    StatementCache<std::unique_ptr<MysqlStmt>> cache_{64};
};

}  // namespace cpppools::db

#else

// mock 模式（CPPPOOLS_USE_MYSQL 关闭）：头文件内容为空。
// mysql_database.cpp 在此模式下也是空 TU，main 不注册 mysql 后端。
// 工厂遇到 type=="mysql" 会返回 "unknown database type"（database_factory.cpp 既有行为）。

#endif
