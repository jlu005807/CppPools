// test_mysql_introspection：MySQL 后端自省专项（spec §十五.2）。
//
// 覆盖 MySQL 特有形态：
//   - 联合唯一 UNIQUE(a,b) 不使任何单列 unique（COLUMN_KEY='UNI' 不得误标）
//   - 前缀唯一索引 UNIQUE(code(10)) 不标 unique（sub_part 非空 → 击穿"每操作至多一行"）
//   - autoincrement 仅显式 AUTO_INCREMENT 才算（MySQL 无 SQLite rowid 魔法）
//   - BLOB/JSON/BIT 列拒绝该数据源（error 指明 table.column）
//   - 生成列/隐藏列排除在 ColumnInfo 之外（insert 全列 SQL 不含该列）
//   - 全类型映射表（int/year/decimal/text/datetime/enum 等）
//
// 仅在 CPPPOOLS_USE_MYSQL 编译时有效。连接信息来自环境变量：
//   CPPPOOLS_TEST_MYSQL_HOST（默认 127.0.0.1）
//   CPPPOOLS_TEST_MYSQL_PORT（默认 3306）
//   CPPPOOLS_TEST_MYSQL_USER（必需）
//   CPPPOOLS_TEST_MYSQL_PASSWORD（可空）
// 任一必需变量缺失则返回 77，由 CTest 归类为 Skipped（spec §十五.3）。
//
// 自动化流程：建临时库 → 建表 → 自省断言 → 删库。库名为 cpppools_intro_test_<pid>，
// 测试结束无论成败都删除。

#include "cpppools/db/database.h"
#include "cpppools/db/mysql_database.h"
#include "cpppools/db/value.h"
#include "db/mysql_database.h"

#include <mysql.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
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
    // user 必需；password 可空（MySQL 允许空密码账号）。
    c.valid = !c.user.empty();
    return c;
}

// ---------------------------------------------------------------------------
// 裸连接：执行单条无结果集语句（DDL）。
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// 建测试 schema：覆盖所有需断言的形态。
// ---------------------------------------------------------------------------
const char* kSchemaDdl[] = {
    "CREATE TABLE users("
    "  id INT PRIMARY KEY AUTO_INCREMENT,"
    "  name VARCHAR(255) NOT NULL,"
    "  age INT,"
    "  score REAL,"
    "  tag VARCHAR(255) UNIQUE,"
    "  created_at DATETIME,"
    "  status ENUM('active','inactive'),"
    "  birth_year YEAR,"
    "  amount DECIMAL(10,2),"
    "  full_name VARCHAR(255) AS (CONCAT(name, ' ', name)) VIRTUAL"
    ")",
    "CREATE TABLE pairs(a INT, b INT, UNIQUE(a, b))",
    "CREATE TABLE nums(id INT PRIMARY KEY AUTO_INCREMENT, v INT)",
    "CREATE TABLE prefix_idx(id INT PRIMARY KEY, code VARCHAR(255), UNIQUE(code(10)))",
    // BLOB 拒绝用单独表（在另一库自省，见下）。
};

// 用于 BLOB 拒绝断言的独立建表语句（单独库，自省时直接失败）。
const char* kBlobDdl = "CREATE TABLE bins(id INT PRIMARY KEY, data BLOB)";
const std::string kQuotedTable = "odd'`name 中文";
const std::string kBackslashTable = "back\\slash 中文";

std::string quote_identifier(const std::string& identifier) {
    std::string quoted = "`";
    for (char ch : identifier) {
        if (ch == '`') {
            quoted += "``";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('`');
    return quoted;
}

// ---------------------------------------------------------------------------
// 临时库 RAII：构造时建库，析构时删库（无论成败）。
// ---------------------------------------------------------------------------
struct TempDatabase {
    MYSQL* root;            // root 连接（不指定 database）
    std::string dbname;
    bool created{false};

    TempDatabase(MYSQL* root_conn, const std::string& name) : root(root_conn), dbname(name) {
        created = exec_ddl(root, "CREATE DATABASE `" + dbname + "`");
    }
    ~TempDatabase() {
        if (created) {
            // 无论测试成败都删库（避免残留影响下次运行）。
            exec_ddl(root, "DROP DATABASE IF EXISTS `" + dbname + "`");
        }
    }
    TempDatabase(const TempDatabase&) = delete;
    TempDatabase& operator=(const TempDatabase&) = delete;
};

// ---------------------------------------------------------------------------
// 断言
// ---------------------------------------------------------------------------

static void test_introspection(const Schema& s, bool has_backslash_table) {
    // 固定表清单加含引号/UTF-8 名称的表；BLOB 表在独立库中拒绝。
    TEST_CHECK(s.has_table("users"));
    TEST_CHECK(s.has_table("pairs"));
    TEST_CHECK(s.has_table("nums"));
    TEST_CHECK(s.has_table("prefix_idx"));
    TEST_CHECK(s.has_table(kQuotedTable));
    TEST_CHECK(s.table_names().size() == (has_backslash_table ? 6u : 5u));

    const TableInfo* quoted = s.find_table(kQuotedTable);
    TEST_CHECK(quoted != nullptr);
    TEST_CHECK(quoted->columns.size() == 2);
    const ColumnInfo* quoted_tag = s.find_column(kQuotedTable, "tag");
    TEST_CHECK(quoted_tag != nullptr && quoted_tag->unique);
    if (has_backslash_table) {
        const TableInfo* backslash = s.find_table(kBackslashTable);
        TEST_CHECK(backslash != nullptr);
        const ColumnInfo* backslash_tag = s.find_column(kBackslashTable, "tag");
        TEST_CHECK(backslash_tag != nullptr && backslash_tag->unique);
    }

    // ---- users ----
    // full_name 是 VIRTUAL 生成列 → 排除在 ColumnInfo 之外。
    // 故 users 列数 = 9（id/name/age/score/tag/created_at/status/birth_year/amount）。
    const TableInfo* users = s.find_table("users");
    TEST_CHECK(users != nullptr);
    TEST_CHECK(users->columns.size() == 9);

    const ColumnInfo* id = s.find_column("users", "id");
    TEST_CHECK(id != nullptr);
    TEST_CHECK(id->kind == ValueKind::kInteger);
    TEST_CHECK(id->primary_key);
    TEST_CHECK(id->unique);            // PK 标 unique
    TEST_CHECK(id->autoincrement);     // 显式 AUTO_INCREMENT
    TEST_CHECK(!id->nullable);

    const ColumnInfo* name = s.find_column("users", "name");
    TEST_CHECK(name != nullptr);
    TEST_CHECK(name->kind == ValueKind::kText);
    TEST_CHECK(!name->nullable);
    TEST_CHECK(!name->unique);

    const ColumnInfo* age = s.find_column("users", "age");
    TEST_CHECK(age != nullptr);
    TEST_CHECK(age->kind == ValueKind::kInteger);
    TEST_CHECK(age->nullable);
    TEST_CHECK(!age->unique);

    const ColumnInfo* score = s.find_column("users", "score");
    TEST_CHECK(score != nullptr);
    TEST_CHECK(score->kind == ValueKind::kReal);

    // tag：单列整列 UNIQUE → unique=true、nullable=true（UNIQUE 允许多 NULL）。
    const ColumnInfo* tag = s.find_column("users", "tag");
    TEST_CHECK(tag != nullptr);
    TEST_CHECK(tag->kind == ValueKind::kText);
    TEST_CHECK(tag->unique);
    TEST_CHECK(tag->nullable);

    // 类型映射：DATETIME/YEAR/ENUM → kText；DECIMAL → kReal。
    const ColumnInfo* created_at = s.find_column("users", "created_at");
    TEST_CHECK(created_at != nullptr);
    TEST_CHECK(created_at->kind == ValueKind::kText);
    const ColumnInfo* status = s.find_column("users", "status");
    TEST_CHECK(status != nullptr);
    TEST_CHECK(status->kind == ValueKind::kText);
    const ColumnInfo* birth_year = s.find_column("users", "birth_year");
    TEST_CHECK(birth_year != nullptr);
    TEST_CHECK(birth_year->kind == ValueKind::kInteger);  // YEAR → kInteger
    const ColumnInfo* amount = s.find_column("users", "amount");
    TEST_CHECK(amount != nullptr);
    TEST_CHECK(amount->kind == ValueKind::kReal);  // DECIMAL → kReal（精度限制已知）

    // 生成列 full_name 被排除：find_column 应返回 nullptr。
    TEST_CHECK(s.find_column("users", "full_name") == nullptr);

    // ---- pairs：联合唯一 UNIQUE(a,b) ----
    // 关键断言：两列在 columns 视图都是 COLUMN_KEY='UNI'，但自省后不得标列级 unique。
    const ColumnInfo* a = s.find_column("pairs", "a");
    const ColumnInfo* b = s.find_column("pairs", "b");
    TEST_CHECK(a != nullptr && b != nullptr);
    TEST_CHECK(!a->unique && "联合唯一不得使单列 unique");
    TEST_CHECK(!b->unique && "联合唯一不得使单列 unique");

    // ---- nums：AUTO_INCREMENT ----
    const ColumnInfo* nv = s.find_column("nums", "v");
    TEST_CHECK(nv != nullptr);
    TEST_CHECK(nv->kind == ValueKind::kInteger);
    TEST_CHECK(nv->nullable);
    TEST_CHECK(!nv->unique);
    const ColumnInfo* nid = s.find_column("nums", "id");
    TEST_CHECK(nid != nullptr);
    TEST_CHECK(nid->autoincrement);

    // ---- prefix_idx：前缀唯一索引 UNIQUE(code(10)) ----
    // 关键断言：前缀索引 sub_part=10 非空 → 不得标 unique（击穿"每操作至多一行"）。
    const ColumnInfo* code = s.find_column("prefix_idx", "code");
    TEST_CHECK(code != nullptr);
    TEST_CHECK(!code->unique && "前缀唯一索引不得标列级 unique");
}

static bool test_session_mode(const TestConn& conn, const std::string& dbname,
                              const std::string& mode, bool has_backslash_table) {
    MYSQL* session = mysql_init(nullptr);
    if (session == nullptr ||
        mysql_real_connect(session, conn.host.c_str(), conn.user.c_str(),
                           conn.password.c_str(), dbname.c_str(),
                           static_cast<unsigned int>(conn.port), nullptr, 0) == nullptr) {
        std::cerr << "[fail] cannot open introspection session: "
                  << (session == nullptr ? "mysql_init failed" : mysql_error(session))
                  << std::endl;
        if (session != nullptr) {
            mysql_close(session);
        }
        return false;
    }
    if (mysql_set_character_set(session, "utf8mb4") != 0) {
        std::cerr << "[fail] cannot set utf8mb4 on introspection session: "
                  << mysql_error(session) << std::endl;
        mysql_close(session);
        return false;
    }

    const std::string set_mode = "SET SESSION sql_mode='" + mode + "'";
    if (mysql_query(session, set_mode.c_str()) != 0) {
        std::cerr << "[fail] cannot set session sql_mode: " << mysql_error(session)
                  << std::endl;
        mysql_close(session);
        return false;
    }
    if (mysql_query(session, "SELECT @@SESSION.sql_mode") != 0) {
        std::cerr << "[fail] cannot read session sql_mode: " << mysql_error(session)
                  << std::endl;
        mysql_close(session);
        return false;
    }
    MYSQL_RES* mode_result = mysql_store_result(session);
    if (mode_result == nullptr) {
        std::cerr << "[fail] cannot store session sql_mode result: " << mysql_error(session)
                  << std::endl;
        mysql_close(session);
        return false;
    }
    MYSQL_ROW mode_row = mysql_fetch_row(mode_result);
    const std::string actual_mode =
        (mode_row != nullptr && mode_row[0] != nullptr) ? mode_row[0] : "";
    mysql_free_result(mode_result);
    if (actual_mode != mode) {
        std::cerr << "[fail] expected session sql_mode='" << mode << "', got '"
                  << actual_mode << "'" << std::endl;
        mysql_close(session);
        return false;
    }
    std::cout << "[info] introspection sql_mode='" << actual_mode << "' server="
              << mysql_get_server_info(session) << " client=" << mysql_get_client_info()
              << std::endl;

    Schema schema;
    std::string error;
    if (!mysql_introspect_schema(session, &schema, &error)) {
        std::cerr << "[fail] introspection failed under sql_mode='" << actual_mode
                  << "': " << error << std::endl;
        mysql_close(session);
        return false;
    }
    if (!error.empty()) {
        std::cerr << "[fail] successful introspection returned error: " << error << std::endl;
        mysql_close(session);
        return false;
    }
    const TableInfo* users = schema.find_table("users");
    const TableInfo* quoted = schema.find_table(kQuotedTable);
    const TableInfo* backslash = has_backslash_table
                                    ? schema.find_table(kBackslashTable)
                                    : nullptr;
    if (users == nullptr || users->columns.size() != 9 || quoted == nullptr ||
        quoted->columns.size() != 2 ||
        (has_backslash_table &&
         (backslash == nullptr || backslash->columns.size() != 2))) {
        std::cerr << "[fail] introspection returned incomplete columns under sql_mode='"
                  << actual_mode << "'" << std::endl;
        mysql_close(session);
        return false;
    }
    test_introspection(schema, has_backslash_table);
    mysql_close(session);
    return true;
}

// BLOB 列拒绝：建一个含 BLOB 的库，自省应失败，error 指明 table.column。
static void test_blob_rejects(const TestConn& conn, const std::string& dbname_prefix) {
    const int pid =
#ifdef _WIN32
        _getpid();
#else
        getpid();
#endif
    const std::string dbname = dbname_prefix + "_blob_" + std::to_string(pid);

    MYSQL* root = mysql_init(nullptr);
    TEST_CHECK(root != nullptr);
    TEST_CHECK(mysql_real_connect(root, conn.host.c_str(), conn.user.c_str(),
                              conn.password.c_str(), nullptr,
                              static_cast<unsigned int>(conn.port), nullptr, 0) != nullptr);
    // 作用域包裹 db：先 DROP DATABASE（db 析构），再 mysql_close(root)。
    // 顺序反了会因连接已关而 DROP 失败，残留测试库。
    {
        TempDatabase db(root, dbname);
        TEST_CHECK(db.created);
        TEST_CHECK(exec_ddl_or_die(root, "USE `" + dbname + "`", "use blob db"));
        TEST_CHECK(exec_ddl_or_die(root, kBlobDdl, "create bins table"));

        config::DataSourceConfig cfg;
        cfg.name = "blob-ds";
        cfg.type = "mysql";
        cfg.mysql_host = conn.host;
        cfg.mysql_port = conn.port;
        cfg.mysql_user = conn.user;
        cfg.mysql_password = conn.password;
        cfg.mysql_database = dbname;
        DatabaseCreateResult result = make_mysql_database(cfg);
        TEST_CHECK(result.db == nullptr);
        TEST_CHECK(result.error.find("bins.data") != std::string::npos);
        TEST_CHECK(result.error.find("blob") != std::string::npos);
    }
    mysql_close(root);
}

}  // namespace

int run_test() {
    const TestConn conn = load_conn();
    if (!conn.valid) {
        std::cout << "[skip] CPPPOOLS_TEST_MYSQL_USER not set; skipping mysql introspection test"
                  << std::endl;
        return kSkipReturnCode;
    }

    const int pid =
#ifdef _WIN32
        _getpid();
#else
        getpid();
#endif
    const std::string dbname = "cpppools_intro_test_" + std::to_string(pid);

    // root 连接：建/删库用（不指定 database，否则 DROP DATABASE 会拒绝）。
    MYSQL* root = mysql_init(nullptr);
    TEST_CHECK(root != nullptr);
    if (mysql_real_connect(root, conn.host.c_str(), conn.user.c_str(),
                           conn.password.c_str(), nullptr,
                           static_cast<unsigned int>(conn.port), nullptr, 0) == nullptr) {
        std::cerr << "[fail] cannot connect to mysql as " << conn.user << "@" << conn.host
                  << ":" << conn.port << ": " << mysql_error(root) << std::endl;
        mysql_close(root);
        return 1;  // configured but unreachable: fail, not skip
    }
    if (mysql_set_character_set(root, "utf8mb4") != 0) {
        std::cerr << "[fail] cannot set utf8mb4 on setup connection: " << mysql_error(root)
                  << std::endl;
        mysql_close(root);
        return 1;
    }

    // 建临时库 + 建表 + 自省断言 + BLOB 拒绝断言，全部在作用域内完成。
    // db（TempDatabase）在作用域结束时析构 DROP DATABASE —— 此时 root 连接仍存活。
    bool sql_modes_ok = true;
    {
        TempDatabase db(root, dbname);
        TEST_CHECK(db.created);
        TEST_CHECK(exec_ddl_or_die(root, "USE `" + dbname + "`", "use test db"));
        for (const char* ddl : kSchemaDdl) {
            TEST_CHECK(exec_ddl_or_die(root, ddl, "create schema table"));
        }
        const std::string quoted_ddl = "CREATE TABLE " + quote_identifier(kQuotedTable) +
            "(id INT PRIMARY KEY, tag VARCHAR(255) UNIQUE) CHARSET=utf8mb4";
        TEST_CHECK(exec_ddl_or_die(root, quoted_ddl, "create quoted UTF-8 table"));

        const std::string backslash_ddl = "CREATE TABLE " + quote_identifier(kBackslashTable) +
            "(id INT PRIMARY KEY, tag VARCHAR(255) UNIQUE) CHARSET=utf8mb4";
        const bool has_backslash_table = exec_ddl(root, backslash_ddl);
        if (!has_backslash_table) {
            std::cout << "[skip] server rejected backslash table identifier: "
                      << mysql_error(root) << std::endl;
        }

        // 用工厂自省（走 make_mysql_database 真实路径）。
        config::DataSourceConfig cfg;
        cfg.name = "intro-ds";
        cfg.type = "mysql";
        cfg.mysql_host = conn.host;
        cfg.mysql_port = conn.port;
        cfg.mysql_user = conn.user;
        cfg.mysql_password = conn.password;
        cfg.mysql_database = dbname;
        DatabaseCreateResult result = make_mysql_database(cfg);
        TEST_CHECK(result.db != nullptr);
        TEST_CHECK(result.error.empty());

        test_introspection(result.db->schema(), has_backslash_table);
        TEST_CHECK(result.db->healthy());

        result.db.reset();  // 先关业务连接，再让 db 析构删库

        sql_modes_ok = test_session_mode(conn, dbname, "", has_backslash_table) &&
                       test_session_mode(conn, dbname, "NO_BACKSLASH_ESCAPES",
                                         has_backslash_table);
    }

    mysql_close(root);
    if (!sql_modes_ok) {
        return 1;
    }

    // BLOB 拒绝断言（独立库，自管连接与删库）。
    test_blob_rejects(conn, "cpppools_intro_test");

    std::cout << "[ok] mysql introspection test passed" << std::endl;
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
