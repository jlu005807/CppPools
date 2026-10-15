// test_mysql_crud：MySQL 后端 CRUD 专项（spec §十七 阶段 3 验收）。
//
// 覆盖 CRUD 关键路径（对齐 SqliteDatabase 的行为契约，但不复制整批 conformance
// 断言 —— spec §十五.1 的 conformance 四遍双跑留待后续抽象）：
//   - insert 往返 + inserted_id（AUTO_INCREMENT）
//   - insert 显式 PK
//   - insert 唯一冲突 → kAlreadyExists（errno 1062 映射）
//   - insert 缺省 NOT NULL 列 → kInvalidRequest（errno 1048 映射）
//   - insert REAL 列收整数（类型规范化）
//   - select 投影 / 全列顺序 / 0 行（kOk + 空集带列名）
//   - update 读改写合并未动列（隐式事务内）
//   - update 改 where 列本身（主键）
//   - update 0 行 kOk
//   - update 唯一冲突 → kAlreadyExists
//   - delete + 再删 0 行
//
// 仅在 CPPPOOLS_USE_MYSQL 编译时有效。连接信息来自环境变量
// （CPPPOOLS_TEST_MYSQL_HOST/PORT/USER/PASSWORD），任一必需变量缺失则返回 77 并由 CTest 跳过。
// 自动化：建临时库 → 建表 → CRUD 断言 → 删库（无论成败）。

#include "cpppools/db/database.h"
#include "cpppools/db/mysql_database.h"
#include "cpppools/db/value.h"
#include "db/mysql_database.h"

#include <mysql.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace cpppools::db;
namespace config = cpppools::config;

namespace {

constexpr int kSkipReturnCode = 77;

void test_check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                                 ": check failed: " + expression);
    }
}

#define TEST_CHECK(expression) \
    test_check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

// ---------------------------------------------------------------------------
// 测试连接配置（来自环境变量）
// ---------------------------------------------------------------------------
struct TestConn {
    std::string host;
    int port{3306};
    std::string user;
    std::string password;
    bool valid{false};
};

TestConn load_conn() {
    TestConn c;
    if (const char* h = std::getenv("CPPPOOLS_TEST_MYSQL_HOST")) {
        c.host = h;
    } else {
        c.host = "127.0.0.1";
    }
    if (const char* p = std::getenv("CPPPOOLS_TEST_MYSQL_PORT")) {
        c.port = std::atoi(p);
    }
    if (const char* u = std::getenv("CPPPOOLS_TEST_MYSQL_USER")) {
        c.user = u;
    }
    if (const char* pw = std::getenv("CPPPOOLS_TEST_MYSQL_PASSWORD")) {
        c.password = pw;
    }
    c.valid = !c.user.empty();
    return c;
}

bool exec_ddl(MYSQL* conn, const std::string& sql) {
    return mysql_query(conn, sql.c_str()) == 0;
}

bool exec_ddl_or_die(MYSQL* conn, const std::string& sql, const char* what) {
    if (!exec_ddl(conn, sql)) {
        std::cerr << "[setup] " << what << " failed: " << mysql_error(conn) << std::endl;
        std::cerr << "  sql: " << sql << std::endl;
        return false;
    }
    return true;
}

// 被测 schema：对齐 SQLite conformance 的 users/pairs/nums 形态，但用 MySQL 方言
// （AUTO_INCREMENT、VARCHAR(255) UNIQUE、NUMERIC 列用 DECIMAL）。
// 显式 CHARSET=utf8mb4：utf8mb4 往返断言依赖表字符集支持 4 字节字符。
const char* kSchemaDdl[] = {
    "CREATE TABLE users("
    "  id INT PRIMARY KEY AUTO_INCREMENT,"
    "  name VARCHAR(255) NOT NULL,"
    "  age INT,"
    "  score REAL,"
    "  tag VARCHAR(255) UNIQUE"
    ") CHARSET=utf8mb4",
    "CREATE TABLE pairs(a INT, b INT, UNIQUE(a, b))",
    "CREATE TABLE nums(id INT PRIMARY KEY AUTO_INCREMENT, v DECIMAL(10,2))",
};

// 临时库 RAII：构造建库、析构删库（无论成败）。
struct TempDatabase {
    MYSQL* root;
    std::string dbname;
    bool created{false};

    TempDatabase(MYSQL* root_conn, const std::string& name) : root(root_conn), dbname(name) {
        created = exec_ddl(root, "CREATE DATABASE `" + dbname + "`");
    }
    ~TempDatabase() {
        if (created) {
            exec_ddl(root, "DROP DATABASE IF EXISTS `" + dbname + "`");
        }
    }
    TempDatabase(const TempDatabase&) = delete;
    TempDatabase& operator=(const TempDatabase&) = delete;
};

WhereClause where_id(std::int64_t v) {
    return WhereClause{"id", Value{v}};
}

InsertRequest make_user(const std::string& name, std::int64_t age, double score,
                        const std::string& tag) {
    return InsertRequest{"users", {{"name", Value{name}},
                                   {"age", Value{age}},
                                   {"score", Value{score}},
                                   {"tag", Value{tag}}}};
}

// ---------------------------------------------------------------------------
// CRUD 断言
// ---------------------------------------------------------------------------

static void test_insert_roundtrip_and_inserted_id(Database& db) {
    const auto r1 = db.insert(make_user("alice", 30, 91.5, "a-1"));
    TEST_CHECK(r1.status == DbStatus::kOk);
    TEST_CHECK(r1.value.affected_rows == 1);
    TEST_CHECK(r1.value.inserted_id.has_value());
    TEST_CHECK(*r1.value.inserted_id == 1);

    const auto r2 = db.insert(make_user("bob", 25, 70.0, "b-1"));
    TEST_CHECK(r2.status == DbStatus::kOk);
    TEST_CHECK(*r2.value.inserted_id == 2);

    // 往返：按生成的 id 查回全列一致。
    SelectRequest sel{"users", {}, where_id(1)};
    const auto got = db.select(sel);
    TEST_CHECK(got.status == DbStatus::kOk);
    TEST_CHECK(got.value.columns.size() == 5);
    TEST_CHECK(got.value.rows.size() == 1);
    const auto& row = got.value.rows[0];
    TEST_CHECK(std::get<std::int64_t>(row[0]) == 1);
    TEST_CHECK(std::get<std::string>(row[1]) == "alice");
    TEST_CHECK(std::get<std::int64_t>(row[2]) == 30);
    TEST_CHECK(std::get<double>(row[3]) == 91.5);
    TEST_CHECK(std::get<std::string>(row[4]) == "a-1");
}

static void test_insert_explicit_pk(Database& db) {
    InsertRequest ins{"users", {{"id", Value{std::int64_t{100}}},
                                {"name", Value{std::string("carol")}}}};
    const auto r = db.insert(ins);
    TEST_CHECK(r.status == DbStatus::kOk);
    TEST_CHECK(r.value.inserted_id.has_value());
    TEST_CHECK(*r.value.inserted_id == 100);  // 显式 PK 也被记录（口径 D12）

    const auto got = db.select(SelectRequest{"users", {"name"}, where_id(100)});
    TEST_CHECK(got.status == DbStatus::kOk);
    TEST_CHECK(got.value.rows.size() == 1);
    TEST_CHECK(std::get<std::string>(got.value.rows[0][0]) == "carol");
}

static void test_insert_unique_conflict(Database& db) {
    // tag 重复 → errno 1062 → kAlreadyExists（口径 D13 → HTTP 409）。
    const auto r = db.insert(make_user("bob", 20, 2.0, "a-1"));
    TEST_CHECK(r.status == DbStatus::kAlreadyExists);
    TEST_CHECK(!r.message.empty());

    // PK 重复同理。
    InsertRequest dup_pk{"users", {{"id", Value{std::int64_t{1}}},
                                   {"name", Value{std::string("x")}}}};
    TEST_CHECK(db.insert(dup_pk).status == DbStatus::kAlreadyExists);
}

static void test_insert_null_and_real_accepts_integer(Database& db) {
    // age 可空：显式 NULL 往返。REAL 列收整数（类型规范化放行）。
    InsertRequest ins{"users", {{"name", Value{std::string("dave")}},
                                {"age", Value{}},
                                {"score", Value{std::int64_t{3}}}}};
    const auto r = db.insert(ins);
    TEST_CHECK(r.status == DbStatus::kOk);
    TEST_CHECK(r.value.inserted_id.has_value());
    const std::int64_t dave_id = *r.value.inserted_id;

    const auto got = db.select(SelectRequest{"users", {"age", "score"},
                                             WhereClause{"id", Value{dave_id}}});
    TEST_CHECK(got.status == DbStatus::kOk);
    const auto& row = got.value.rows[0];
    TEST_CHECK(is_null(row[0]));
    TEST_CHECK(std::get<double>(row[1]) == 3.0);
}

static void test_insert_missing_notnull_is_invalid_request(Database& db) {
    // 缺 name（NOT NULL 无 DEFAULT）→ errno 1048 → kInvalidRequest（HTTP 400）。
    InsertRequest ins{"users", {{"age", Value{std::int64_t{1}}}}};
    const auto r = db.insert(ins);
    TEST_CHECK(r.status == DbStatus::kInvalidRequest);
    TEST_CHECK(r.message.find("constraint violation") != std::string::npos);
}

static void test_select_projection_and_empty(Database& db) {
    // 投影：请求顺序 = 返回顺序。
    const auto proj = db.select(SelectRequest{"users", {"age", "name"}, where_id(1)});
    TEST_CHECK(proj.status == DbStatus::kOk);
    TEST_CHECK(proj.value.columns.size() == 2);
    TEST_CHECK(proj.value.columns[0] == "age");
    TEST_CHECK(proj.value.columns[1] == "name");

    // 全列：schema 顺序。
    const auto full = db.select(SelectRequest{"users", {}, where_id(1)});
    TEST_CHECK(full.value.columns.size() == 5);
    TEST_CHECK(full.value.columns[0] == "id");
    TEST_CHECK(full.value.columns[4] == "tag");

    // 0 行：kOk + 空集仍带列名。
    const auto none = db.select(SelectRequest{"users", {}, where_id(999)});
    TEST_CHECK(none.status == DbStatus::kOk);
    TEST_CHECK(none.value.rows.empty());
    TEST_CHECK(none.value.columns.size() == 5 && "空集仍带列名");
}

static void test_update_merges_untouched_columns(Database& db) {
    // 只改 score；其余列必须保持（读改写，隐式事务内）。
    UpdateRequest upd{"users", {{"score", Value{99.0}}}, where_id(1)};
    const auto r = db.update(upd);
    TEST_CHECK(r.status == DbStatus::kOk);
    TEST_CHECK(r.value == 1);

    const auto got = db.select(SelectRequest{"users", {}, where_id(1)});
    const auto& row = got.value.rows[0];
    TEST_CHECK(std::get<std::string>(row[1]) == "alice");   // 未动
    TEST_CHECK(std::get<std::int64_t>(row[2]) == 30);       // 未动
    TEST_CHECK(std::get<double>(row[3]) == 99.0);           // 已改
    TEST_CHECK(std::get<std::string>(row[4]) == "a-1");     // 未动
}

static void test_update_unique_conflict(Database& db) {
    // 把 bob 的 tag 改成 alice 已占用的值 → kAlreadyExists。
    UpdateRequest upd{"users", {{"tag", Value{std::string("a-1")}}}, where_id(2)};
    const auto r = db.update(upd);
    TEST_CHECK(r.status == DbStatus::kAlreadyExists);
    TEST_CHECK(!r.message.empty());
}

static void test_update_can_change_where_column(Database& db) {
    // 改主键 id 1 → 200：读改写 SET 全集包含 where 列本身。
    UpdateRequest upd{"users", {{"id", Value{std::int64_t{200}}}}, where_id(1)};
    const auto r = db.update(upd);
    TEST_CHECK(r.status == DbStatus::kOk);
    TEST_CHECK(r.value == 1);

    TEST_CHECK(db.select(SelectRequest{"users", {}, where_id(1)}).value.rows.empty());
    const auto got = db.select(SelectRequest{"users", {"name"}, where_id(200)});
    TEST_CHECK(got.status == DbStatus::kOk);
    TEST_CHECK(got.value.rows.size() == 1);
    TEST_CHECK(std::get<std::string>(got.value.rows[0][0]) == "alice");
}

static void test_update_zero_rows_is_ok(Database& db) {
    UpdateRequest upd{"users", {{"score", Value{1.0}}}, where_id(404)};
    const auto r = db.update(upd);
    TEST_CHECK(r.status == DbStatus::kOk);
    TEST_CHECK(r.value == 0);  // 决策②：0 行影响不是错误
}

static void test_delete_and_zero_rows(Database& db) {
    // 删掉改了 id 的 alice（现在 id=200）。
    const auto r = db.remove(DeleteRequest{"users", where_id(200)});
    TEST_CHECK(r.status == DbStatus::kOk);
    TEST_CHECK(r.value == 1);

    const auto gone = db.select(SelectRequest{"users", {}, where_id(200)});
    TEST_CHECK(gone.value.rows.empty());

    // 再删：0 行，kOk。
    const auto r2 = db.remove(DeleteRequest{"users", where_id(200)});
    TEST_CHECK(r2.status == DbStatus::kOk);
    TEST_CHECK(r2.value == 0);
}

// nums：DECIMAL 列存整数读回应提升为 double（与 SQLite NUMERIC 对齐）。
static void test_decimal_column_read_promotes_to_double(Database& db) {
    InsertRequest ins{"nums", {{"v", Value{std::int64_t{7}}}}};
    const auto r = db.insert(ins);
    TEST_CHECK(r.status == DbStatus::kOk);
    TEST_CHECK(r.value.inserted_id.has_value());
    const std::int64_t nid = *r.value.inserted_id;
    const auto got = db.select(SelectRequest{"nums", {"v"},
                                             WhereClause{"id", Value{nid}}});
    TEST_CHECK(got.status == DbStatus::kOk);
    TEST_CHECK(std::holds_alternative<double>(got.value.rows[0][0]) &&
           "DECIMAL 列的整数必须提升为 double");
    TEST_CHECK(std::get<double>(got.value.rows[0][0]) == 7.0);
}

// utf8mb4 往返（spec §十一 契约点）：中文 + emoji 多字节字符，验证字符集
// 正确设置且 TEXT 列读写无损。若未 set_character_set("utf8mb4")，emoji 会乱码。
static void test_utf8mb4_roundtrip(Database& db) {
    const std::string name = "测试'用户\\path🚀";  // 引号、反斜杠、中文与 4 字节 emoji
    InsertRequest ins{"users", {{"name", Value{name}}, {"tag", Value{std::string("utf8-1")}}}};
    const auto r = db.insert(ins);
    TEST_CHECK(r.status == DbStatus::kOk);
    const std::int64_t uid = *r.value.inserted_id;
    const auto got = db.select(SelectRequest{"users", {"name"},
                                             WhereClause{"id", Value{uid}}});
    TEST_CHECK(got.status == DbStatus::kOk);
    TEST_CHECK(std::get<std::string>(got.value.rows[0][0]) == name);
}

static void run_crud_suite(Database& db) {
    test_insert_roundtrip_and_inserted_id(db);
    test_insert_explicit_pk(db);
    test_insert_unique_conflict(db);
    test_insert_null_and_real_accepts_integer(db);
    test_insert_missing_notnull_is_invalid_request(db);
    test_select_projection_and_empty(db);
    test_update_merges_untouched_columns(db);
    test_update_unique_conflict(db);
    test_update_can_change_where_column(db);
    test_update_zero_rows_is_ok(db);
    test_delete_and_zero_rows(db);
    test_decimal_column_read_promotes_to_double(db);
    test_utf8mb4_roundtrip(db);
}

static bool run_session_crud(const TestConn& conn, const std::string& dbname,
                             const std::string& mode) {
    MYSQL* root = mysql_init(nullptr);
    if (root == nullptr ||
        mysql_real_connect(root, conn.host.c_str(), conn.user.c_str(),
                           conn.password.c_str(), nullptr,
                           static_cast<unsigned int>(conn.port), nullptr, 0) == nullptr) {
        std::cerr << "[fail] cannot open CRUD setup connection: "
                  << (root == nullptr ? "mysql_init failed" : mysql_error(root)) << std::endl;
        if (root != nullptr) {
            mysql_close(root);
        }
        return false;
    }
    if (mysql_set_character_set(root, "utf8mb4") != 0) {
        std::cerr << "[fail] cannot set utf8mb4 on CRUD setup connection: "
                  << mysql_error(root) << std::endl;
        mysql_close(root);
        return false;
    }

    bool ok = true;
    {
        TempDatabase temp(root, dbname);
        if (!temp.created) {
            std::cerr << "[fail] cannot create CRUD mode database " << dbname << ": "
                      << mysql_error(root) << std::endl;
            ok = false;
        } else if (!exec_ddl_or_die(root, "USE `" + dbname + "`", "use CRUD mode db")) {
            ok = false;
        } else {
            for (const char* ddl : kSchemaDdl) {
                if (!exec_ddl_or_die(root, ddl, "create CRUD mode schema table")) {
                    ok = false;
                    break;
                }
            }

            if (ok) {
                MYSQL* session = mysql_init(nullptr);
                if (session == nullptr ||
                    mysql_real_connect(session, conn.host.c_str(), conn.user.c_str(),
                                       conn.password.c_str(), dbname.c_str(),
                                       static_cast<unsigned int>(conn.port), nullptr, 0) == nullptr) {
                    std::cerr << "[fail] cannot open CRUD session: "
                              << (session == nullptr ? "mysql_init failed" : mysql_error(session))
                              << std::endl;
                    if (session != nullptr) {
                        mysql_close(session);
                    }
                    ok = false;
                } else if (mysql_set_character_set(session, "utf8mb4") != 0) {
                    std::cerr << "[fail] cannot set utf8mb4 on CRUD session: "
                              << mysql_error(session) << std::endl;
                    mysql_close(session);
                    ok = false;
                } else {
                    const std::string set_mode = "SET SESSION sql_mode='" + mode + "'";
                    if (mysql_query(session, set_mode.c_str()) != 0) {
                        std::cerr << "[fail] cannot set CRUD session sql_mode: "
                                  << mysql_error(session) << std::endl;
                        mysql_close(session);
                        ok = false;
                    } else {
                        std::string error;
                        Schema schema;
                        if (!mysql_introspect_schema(session, &schema, &error)) {
                            std::cerr << "[fail] CRUD introspection failed under sql_mode='"
                                      << mode << "': " << error << std::endl;
                            mysql_close(session);
                            ok = false;
                        } else {
                            const TableInfo* users = schema.find_table("users");
                            if (users == nullptr || users->columns.size() != 5) {
                                std::cerr << "[fail] CRUD introspection returned incomplete users "
                                          << "columns under sql_mode='" << mode << "'"
                                          << std::endl;
                                mysql_close(session);
                                ok = false;
                            } else {
                                std::string actual_mode;
                                bool mode_read = mysql_query(session,
                                                             "SELECT @@SESSION.sql_mode") == 0;
                                MYSQL_RES* result = mode_read ? mysql_store_result(session) : nullptr;
                                mode_read = mode_read && result != nullptr;
                                if (mode_read) {
                                    MYSQL_ROW row = mysql_fetch_row(result);
                                    mode_read = row != nullptr && row[0] != nullptr;
                                    if (mode_read) {
                                        actual_mode = row[0];
                                    }
                                }
                                if (result != nullptr) {
                                    mysql_free_result(result);
                                }
                                if (!mode_read || actual_mode != mode) {
                                    std::cerr << "[fail] expected CRUD session sql_mode='" << mode
                                              << "', got '" << actual_mode << "'" << std::endl;
                                    mysql_close(session);
                                    ok = false;
                                } else {
                                    std::cout << "[info] CRUD sql_mode='" << actual_mode
                                              << "' server=" << mysql_get_server_info(session)
                                              << " client=" << mysql_get_client_info() << std::endl;
                                    MysqlDatabase db(session, std::move(schema), "crud-session");
                                    if (!db.healthy()) {
                                        std::cerr << "[fail] CRUD session database is unhealthy"
                                                  << std::endl;
                                        ok = false;
                                    } else {
                                        run_crud_suite(db);
                                    }
                                    // MysqlDatabase now owns and closes session.
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    mysql_close(root);
    return ok;
}

}  // namespace

int run_test() {
    const TestConn conn = load_conn();
    if (!conn.valid) {
        std::cout << "[skip] CPPPOOLS_TEST_MYSQL_USER not set; skipping mysql crud test"
                  << std::endl;
        return kSkipReturnCode;
    }

    const int pid =
#ifdef _WIN32
        _getpid();
#else
        getpid();
#endif
    const std::string dbname = "cpppools_crud_test_" + std::to_string(pid);

    MYSQL* root = mysql_init(nullptr);
    TEST_CHECK(root != nullptr);
    if (mysql_real_connect(root, conn.host.c_str(), conn.user.c_str(),
                           conn.password.c_str(), nullptr,
                           static_cast<unsigned int>(conn.port), nullptr, 0) == nullptr) {
        std::cerr << "[fail] cannot connect to mysql as " << conn.user << "@" << conn.host
                  << ":" << conn.port << ": " << mysql_error(root) << std::endl;
        mysql_close(root);
        return 1;
    }

    // 建临时库 + 建表 + CRUD 断言，全部在作用域内完成。
    // db（TempDatabase）在作用域结束时析构 DROP DATABASE —— 此时 root 仍存活。
    {
        TempDatabase db_scope(root, dbname);
        TEST_CHECK(db_scope.created);
        TEST_CHECK(exec_ddl_or_die(root, "USE `" + dbname + "`", "use crud test db"));
        for (const char* ddl : kSchemaDdl) {
            TEST_CHECK(exec_ddl_or_die(root, ddl, "create crud schema table"));
        }

        config::DataSourceConfig cfg;
        cfg.name = "crud-ds";
        cfg.type = "mysql";
        cfg.mysql_host = conn.host;
        cfg.mysql_port = conn.port;
        cfg.mysql_user = conn.user;
        cfg.mysql_password = conn.password;
        cfg.mysql_database = dbname;
        DatabaseCreateResult result = make_mysql_database(cfg);
        TEST_CHECK(result.db != nullptr);
        TEST_CHECK(result.error.empty());

        Database& db = *result.db;
        TEST_CHECK(db.healthy());
        run_crud_suite(db);

        result.db.reset();  // 先关业务连接，再让 db_scope 析构删库
    }

    mysql_close(root);

    if (!run_session_crud(conn, dbname + "_empty", "") ||
        !run_session_crud(conn, dbname + "_nbe", "NO_BACKSLASH_ESCAPES")) {
        return 1;
    }

    std::cout << "[ok] mysql crud test passed" << std::endl;
    return 0;
}

int main() {
    try {
        return run_test();
    } catch (const std::exception& error) {
        std::cerr << "[fail] " << error.what() << std::endl;
        return 1;
    }
}
