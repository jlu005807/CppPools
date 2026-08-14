#include "cpppools/db/database.h"
#include "cpppools/db/database_factory.h"
#include "cpppools/db/row_request.h"
#include "cpppools/db/schema.h"
#include "cpppools/db/value.h"

#include <cassert>
#include <memory>
#include <optional>
#include <string>

using namespace cpppools::db;

namespace {

// 测试用的假数据库：记录 do_* 收到的请求（验证透传），不做任何真实操作。
// 走 Database 的 public select/insert/update/remove —— 即完整穿过 NVI 校验管道。
class FakeDatabase : public Database {
public:
    FakeDatabase(Schema schema, std::string name)
        : Database(std::move(schema), std::move(name)) {}

    mutable int select_calls{0};
    mutable int insert_calls{0};
    mutable int update_calls{0};
    mutable int remove_calls{0};
    mutable SelectRequest last_select;
    mutable InsertRequest last_insert;

    bool healthy() override { return true; }
    std::string backend_type() const override { return "fake"; }

protected:
    DbResult<ResultSet> do_select(const SelectRequest& req) override {
        ++select_calls;
        last_select = req;
        return {DbStatus::kOk, ResultSet{}, ""};
    }
    DbResult<InsertResult> do_insert(const InsertRequest& req) override {
        ++insert_calls;
        last_insert = req;
        return {DbStatus::kOk, InsertResult{1, std::nullopt}, ""};
    }
    DbResult<RowCount> do_update(const UpdateRequest&) override {
        ++update_calls;
        return {DbStatus::kOk, 1, ""};
    }
    DbResult<RowCount> do_remove(const DeleteRequest&) override {
        ++remove_calls;
        return {DbStatus::kOk, 1, ""};
    }
};

// 测试 schema：
//   users(id INTEGER PK+autoincrement, name TEXT NOT NULL, age INTEGER NULL,
//         score REAL NOT NULL, rating REAL UNIQUE)
//   logs(msg TEXT NOT NULL)   —— msg 非唯一
Schema make_test_schema() {
    Schema s;
    TableInfo users;
    users.name = "users";
    users.columns = {
        {"id", ValueKind::kInteger, false, true, true, true},
        {"name", ValueKind::kText, false, false, false, false},
        {"age", ValueKind::kInteger, true, false, false, false},
        {"score", ValueKind::kReal, false, false, false, false},
        {"rating", ValueKind::kReal, false, false, true, false},
    };
    TableInfo logs;
    logs.name = "logs";
    logs.columns = {{"msg", ValueKind::kText, false, false, false, false}};
    s.add_table(std::move(users));
    s.add_table(std::move(logs));
    return s;
}

WhereClause where_id(const Value& v) {
    return WhereClause{"id", v};
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// Schema 查询
// ---------------------------------------------------------------------------

static void test_schema_lookup() {
    const Schema s = make_test_schema();
    assert(s.has_table("users"));
    assert(s.has_table("logs"));
    assert(!s.has_table("ghost"));
    assert(!s.has_table("USERS"));  // 精确匹配，不做大小写规范化

    const TableInfo* t = s.find_table("users");
    assert(t != nullptr);
    assert(t->columns.size() == 5);

    const ColumnInfo* c = s.find_column("users", "name");
    assert(c != nullptr);
    assert(c->kind == ValueKind::kText);
    assert(!c->nullable);
    assert(!c->unique);

    const ColumnInfo* id = s.find_column("users", "id");
    assert(id != nullptr);
    assert(id->primary_key);
    assert(id->unique);
    assert(id->autoincrement);

    assert(s.find_column("users", "ghost") == nullptr);
    assert(s.find_column("ghost", "id") == nullptr);

    const auto names = s.table_names();
    assert(names.size() == 2);
}

// ---------------------------------------------------------------------------
// 校验管道（NVI）：合法请求必须穿过到 do_*，且内容原样透传
// ---------------------------------------------------------------------------

static void test_valid_requests_pass_validation() {
    FakeDatabase db(make_test_schema(), "fake");

    SelectRequest sel{"users", {"name", "age"}, where_id(int64_t{42})};
    assert(db.select(sel).status == DbStatus::kOk);
    assert(db.select_calls == 1);

    // select 不带 columns = 全部列。
    SelectRequest sel_all{"users", {}, where_id(int64_t{1})};
    assert(db.select(sel_all).status == DbStatus::kOk);
    assert(db.select_calls == 2);

    InsertRequest ins{"users", {{"name", Value{std::string("alice")}},
                                {"age", Value{int64_t{30}}},
                                {"score", Value{3.5}}}};
    const auto ins_r = db.insert(ins);
    assert(ins_r.status == DbStatus::kOk);
    assert(db.insert_calls == 1);
    assert(!ins_r.value.inserted_id.has_value());  // FakeDatabase 无自增键

    // 显式传自增列是合法路径（客户端指定主键）。
    InsertRequest ins_with_pk{"users", {{"id", Value{int64_t{99}}},
                                        {"name", Value{std::string("bob")}}}};
    assert(db.insert(ins_with_pk).status == DbStatus::kOk);

    UpdateRequest upd{"users", {{"score", Value{4.0}}}, where_id(int64_t{42})};
    assert(db.update(upd).status == DbStatus::kOk);
    assert(db.update_calls == 1);

    DeleteRequest del{"users", where_id(int64_t{42})};
    assert(db.remove(del).status == DbStatus::kOk);
    assert(db.remove_calls == 1);

    // 透传：do_* 收到的请求内容未被管道篡改。
    assert(db.last_select.table == "users");
    assert(db.last_select.columns.empty());
    assert(db.last_select.where.column == "id");
    assert(std::holds_alternative<std::int64_t>(db.last_select.where.value));
    assert(db.last_insert.table == "users");
    assert(db.last_insert.values.size() == 2);
    assert(db.last_insert.values[0].first == "id");
}

// ---------------------------------------------------------------------------
// 校验管道：非法请求必须在进入 do_* 之前被拦下（kInvalidRequest）
// ---------------------------------------------------------------------------

static void test_unknown_table_rejected() {
    FakeDatabase db(make_test_schema(), "fake");

    SelectRequest sel{"ghost", {"name"}, where_id(int64_t{1})};
    const auto r = db.select(sel);
    assert(r.status == DbStatus::kInvalidRequest);
    assert(contains(r.message, "unknown table"));
    assert(contains(r.message, "ghost"));
    assert(db.select_calls == 0 && "非法请求不得进入 do_select");
}

static void test_unknown_column_rejected() {
    FakeDatabase db(make_test_schema(), "fake");

    SelectRequest sel{"users", {"ghost"}, where_id(int64_t{1})};
    const auto r1 = db.select(sel);
    assert(r1.status == DbStatus::kInvalidRequest);
    assert(contains(r1.message, "unknown column"));
    assert(db.select_calls == 0);

    InsertRequest ins{"users", {{"ghost", Value{int64_t{1}}}}};
    assert(db.insert(ins).status == DbStatus::kInvalidRequest);
    assert(db.insert_calls == 0);

    UpdateRequest upd{"users", {{"ghost", Value{int64_t{1}}}}, where_id(int64_t{1})};
    assert(db.update(upd).status == DbStatus::kInvalidRequest);
    assert(db.update_calls == 0);
}

static void test_where_column_must_be_unique() {
    // 核心安全性质：where 必须是 PK/UNIQUE —— 中间件在结构上无法被用来批量改删。
    FakeDatabase db(make_test_schema(), "fake");

    // logs.msg 不是唯一键。
    DeleteRequest del{"logs", WhereClause{"msg", Value{std::string("x")}}};
    const auto r = db.remove(del);
    assert(r.status == DbStatus::kInvalidRequest);
    assert(contains(r.message, "primary key or unique"));
    assert(db.remove_calls == 0);

    UpdateRequest upd{"logs", {{"msg", Value{std::string("y")}}},
                      WhereClause{"msg", Value{std::string("x")}}};
    assert(db.update(upd).status == DbStatus::kInvalidRequest);
    assert(db.update_calls == 0);

    SelectRequest sel{"logs", {"msg"}, WhereClause{"msg", Value{std::string("x")}}};
    assert(db.select(sel).status == DbStatus::kInvalidRequest);
    assert(db.select_calls == 0);
}

static void test_type_mismatch_rejected() {
    // 严格类型：不做隐式转换（双后端一致性测试的前提）。
    FakeDatabase db(make_test_schema(), "fake");

    // name 是 TEXT，给整数 → 拒绝。
    InsertRequest ins{"users", {{"name", Value{int64_t{42}}}}};
    const auto r = db.insert(ins);
    assert(r.status == DbStatus::kInvalidRequest);
    assert(contains(r.message, "expects string"));
    assert(db.insert_calls == 0);

    // id 是 INTEGER，给字符串 → 拒绝。
    SelectRequest sel{"users", {"name"}, WhereClause{"id", Value{std::string("42")}}};
    assert(db.select(sel).status == DbStatus::kInvalidRequest);
    assert(db.select_calls == 0);

    // score 是 REAL：整数允许（整数可无损表示为实数），字符串拒绝。
    UpdateRequest ok_upd{"users", {{"score", Value{int64_t{4}}}}, where_id(int64_t{1})};
    assert(db.update(ok_upd).status == DbStatus::kOk);
    UpdateRequest bad_upd{"users", {{"score", Value{std::string("4")}}}, where_id(int64_t{1})};
    assert(db.update(bad_upd).status == DbStatus::kInvalidRequest);

    // where 侧同样放行整数→REAL（rating 是唯一 REAL 列）。
    SelectRequest rating_sel{"users", {"name"}, WhereClause{"rating", Value{int64_t{4}}}};
    assert(db.select(rating_sel).status == DbStatus::kOk);

    // INTEGER 列收到小数 → 拒绝（不做截断）。
    InsertRequest frac{"users", {{"age", Value{3.5}}}};
    assert(db.insert(frac).status == DbStatus::kInvalidRequest);
}

static void test_null_only_for_nullable_columns() {
    FakeDatabase db(make_test_schema(), "fake");

    // age 可空：NULL 允许。
    InsertRequest ok_ins{"users", {{"age", Value{}}}};
    assert(db.insert(ok_ins).status == DbStatus::kOk);

    // name NOT NULL：NULL 拒绝。
    InsertRequest bad_ins{"users", {{"name", Value{}}}};
    const auto r = db.insert(bad_ins);
    assert(r.status == DbStatus::kInvalidRequest);
    assert(contains(r.message, "does not accept NULL"));

    // where 值不得为 NULL。
    SelectRequest sel{"users", {"name"}, WhereClause{"id", Value{}}};
    const auto r2 = db.select(sel);
    assert(r2.status == DbStatus::kInvalidRequest);
    assert(contains(r2.message, "must not be NULL"));
}

static void test_empty_requests_rejected() {
    FakeDatabase db(make_test_schema(), "fake");

    InsertRequest empty_ins{"users", {}};
    assert(db.insert(empty_ins).status == DbStatus::kInvalidRequest);

    UpdateRequest empty_upd{"users", {}, where_id(int64_t{1})};
    assert(db.update(empty_upd).status == DbStatus::kInvalidRequest);

    // 表名为空。
    SelectRequest no_table{"", {"name"}, where_id(int64_t{1})};
    assert(db.select(no_table).status == DbStatus::kInvalidRequest);
}

static void test_duplicate_column_rejected() {
    // 重复列是客户端契约违反：拦在校验层（400），不让后端以 500 暴露。
    FakeDatabase db(make_test_schema(), "fake");

    InsertRequest dup_ins{"users", {{"name", Value{std::string("a")}},
                                    {"name", Value{std::string("b")}}}};
    const auto r = db.insert(dup_ins);
    assert(r.status == DbStatus::kInvalidRequest);
    assert(contains(r.message, "duplicate column"));
    assert(db.insert_calls == 0);

    UpdateRequest dup_upd{"users", {{"score", Value{1.0}}, {"score", Value{2.0}}},
                          where_id(int64_t{1})};
    assert(db.update(dup_upd).status == DbStatus::kInvalidRequest);
    assert(db.update_calls == 0);
}

// ---------------------------------------------------------------------------
// 工厂注册表
// ---------------------------------------------------------------------------

static void test_factory_register_create_unregister() {
    const std::string type = "unittest-fake";
    assert(register_database_type(
        type, [](const cpppools::config::DataSourceConfig& cfg) -> DatabaseCreateResult {
            return {std::make_unique<FakeDatabase>(make_test_schema(), cfg.name), ""};
        }));
    // 重复注册同类型 → 失败。
    assert(!register_database_type(
        type, [](const cpppools::config::DataSourceConfig&) -> DatabaseCreateResult {
            return {nullptr, "dup"};
        }));

    cpppools::config::DataSourceConfig cfg;
    cfg.name = "t";
    cfg.type = type;
    DatabaseCreateResult result = create_database(cfg);
    assert(result.db != nullptr);
    assert(result.error.empty());
    assert(result.db->schema().has_table("users"));
    assert(result.db->name() == "t");

    assert(unregister_database_type(type));
    assert(!unregister_database_type(type));  // 已注销
    const DatabaseCreateResult missing = create_database(cfg);
    assert(missing.db == nullptr);            // 未知类型 → error 非空
    assert(contains(missing.error, "unknown database type"));
}

static void test_factory_error_propagates() {
    // 工厂失败：error 原样传回。
    const std::string type = "unittest-failing";
    assert(register_database_type(
        type, [](const cpppools::config::DataSourceConfig&) -> DatabaseCreateResult {
            return {nullptr, "cannot open db file"};
        }));
    cpppools::config::DataSourceConfig cfg;
    cfg.type = type;
    const DatabaseCreateResult result = create_database(cfg);
    assert(result.db == nullptr);
    assert(result.error == "cannot open db file");
    unregister_database_type(type);

    // 静默失败防御：工厂返回空且无 error → create_database 补 error。
    const std::string silent_type = "unittest-silent";
    assert(register_database_type(
        silent_type, [](const cpppools::config::DataSourceConfig&) -> DatabaseCreateResult {
            return {nullptr, ""};
        }));
    cfg.type = silent_type;
    const DatabaseCreateResult silent = create_database(cfg);
    assert(silent.db == nullptr);
    assert(!silent.error.empty());
    unregister_database_type(silent_type);
}

int main() {
    test_schema_lookup();
    test_valid_requests_pass_validation();
    test_unknown_table_rejected();
    test_unknown_column_rejected();
    test_where_column_must_be_unique();
    test_type_mismatch_rejected();
    test_null_only_for_nullable_columns();
    test_empty_requests_rejected();
    test_duplicate_column_rejected();
    test_factory_register_create_unregister();
    test_factory_error_propagates();
    return 0;
}
