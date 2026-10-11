// 通用行级 CRUD 端点（spec §7）：JSON body → 路由 → 校验管道 → 状态码矩阵。
//
// nlohmann/json 只在本 .cpp 出现（公开头文件不引 2.4 万行的第三方头）。

#include "server/crud_api.h"

#include "server/ownership_router.h"

#include "cpppools/db/database.h"
#include "cpppools/db/datasource_registry.h"
#include "cpppools/db/db_connection_pool.h"
#include "cpppools/db/row_request.h"
#include "cpppools/db/value.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cpppools::server {

using db::Database;
using db::DbConnGuard;
using db::DbConnectionPool;
using db::DbResult;
using db::DbStatus;
using db::DeleteRequest;
using db::InsertRequest;
using db::InsertResult;
using db::RowCount;
using db::RouteResult;
using db::RouteStatus;
using db::ResultSet;
using db::SelectRequest;
using db::UpdateRequest;
using db::Value;
using db::WhereClause;
using njson = nlohmann::json;

namespace {

ApiResponse json_response(int status_code, std::string status_text, std::string body) {
    ApiResponse resp;
    resp.status_code = status_code;
    resp.status_text = std::move(status_text);
    resp.body = std::move(body);
    return resp;
}

ApiResponse bad_request(std::string message) {
    return make_error_response(400, "Bad Request", std::move(message));
}

// JSON 解析的统一入口：body 必须是 JSON **对象**。
std::optional<njson> parse_body_object(const CrudApiContext& ctx, ApiResponse* error_out) {
    if (ctx.body.empty()) {
        *error_out = bad_request("request body is required (JSON)");
        return std::nullopt;
    }
    njson doc;
    try {
        doc = njson::parse(ctx.body.begin(), ctx.body.end());
    } catch (const njson::exception& ex) {
        // catch 基类而非仅 parse_error：合法语法但溢出的数字字面量
        // （如 1e999）会抛 out_of_range.406 —— 同样是输入问题，必须回 400
        // 而不是穿出 handler 被兜成 500。bad_alloc 类内存失败仍留给外层兜底。
        *error_out = bad_request(std::string("malformed JSON: ") + ex.what());
        return std::nullopt;
    }
    if (!doc.is_object()) {
        *error_out = bad_request("request body must be a JSON object");
        return std::nullopt;
    }
    return doc;
}

// JSON 值 → 中间件 Value（严格：只收 int/float/string/null）。
// 布尔/对象/数组拒绝 —— 中间件不猜语义（决策④的同源精神）。
bool json_to_value(const njson& j, Value* out, std::string* error) {
    if (j.is_null()) {
        *out = Value{};
        return true;
    }
    if (j.is_number_integer() || j.is_number_unsigned()) {
        if (j.is_number_unsigned()) {
            const auto u = j.get<std::uint64_t>();
            if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                *error = "integer value out of range (exceeds INT64_MAX)";
                return false;
            }
        }
        *out = Value{j.get<std::int64_t>()};
        return true;
    }
    if (j.is_number_float()) {
        *out = Value{j.get<double>()};
        return true;
    }
    if (j.is_string()) {
        *out = Value{j.get<std::string>()};
        return true;
    }
    *error = "unsupported JSON value type (expected number, string or null)";
    return false;
}

// 列名→值 对象（insert 的 values / update 的 set）。
bool parse_column_values(const njson& obj,
                         std::vector<std::pair<std::string, Value>>* out,
                         std::string* error) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        Value v;
        if (!json_to_value(it.value(), &v, error)) {
            *error = "column \"" + it.key() + "\": " + *error;
            return false;
        }
        out->emplace_back(it.key(), std::move(v));
    }
    return true;
}

// where 子句：{"column": str, "value": any}。
bool parse_where(const njson& doc, WhereClause* out, std::string* error) {
    const auto wit = doc.find("where");
    if (wit == doc.end() || !wit->is_object()) {
        *error = "missing or invalid \"where\" object";
        return false;
    }
    const auto cit = wit->find("column");
    if (cit == wit->end() || !cit->is_string()) {
        *error = "\"where.column\" must be a string";
        return false;
    }
    const auto vit = wit->find("value");
    if (vit == wit->end()) {
        *error = "\"where.value\" is required";
        return false;
    }
    out->column = cit->get<std::string>();
    return json_to_value(*vit, &out->value, error);
}

// 必填字符串字段。
bool require_string(const njson& doc, const char* key, std::string* out,
                    std::string* error) {
    const auto it = doc.find(key);
    if (it == doc.end() || !it->is_string() || it->get<std::string>().empty()) {
        *error = std::string("\"") + key + "\" is required (non-empty string)";
        return false;
    }
    *out = it->get<std::string>();
    return true;
}

// 可选 datasource 字段。返回 nullopt = 未指定；false = 类型错（需回 400）。
bool optional_datasource(const njson& doc, std::string* out, bool* type_error) {
    *type_error = false;
    const auto it = doc.find("datasource");
    if (it == doc.end()) {
        return true;
    }
    if (!it->is_string()) {
        *type_error = true;
        return false;
    }
    *out = it->get<std::string>();
    return true;
}

// 路由结果 → HTTP：歧义 409（候选写进 message），未知表/源 400（spec §7.3）。
ApiResponse map_route_error(const RouteResult& route) {
    if (route.status == RouteStatus::kAmbiguous) {
        return make_error_response(409, "Conflict", route.message);
    }
    return bad_request(route.message);
}

// DbStatus → HTTP（spec §7.3）：校验错 400；唯一冲突 409；后端错 500 透传消息。
// kNotFound 当前无后端返回（0 行走 kOk+affected_rows:0 的 SQL 语义）；
// 若未来有后端使用，必须先在此定义 HTTP 语义，不得静默落入 500 分支。
ApiResponse map_db_error(DbStatus status, const std::string& message) {
    if (status == DbStatus::kAlreadyExists) {
        return make_error_response(409, "Conflict", message);
    }
    if (status == DbStatus::kInvalidRequest) {
        return bad_request(message);
    }
    return make_error_response(500, "Internal Server Error",
                               message.empty() ? "database operation failed" : message);
}

// 容错序列化：外部写入的 TEXT 可能不是合法 UTF-8（中间件不垄断写入），
// dump 默认严格校验会抛 type_error.316 → 一条脏数据让整行查询永久 500。
// replace 策略把非法字节换成 U+FFFD：查询仍可用，脏数据可辨认。
std::string safe_dump(const njson& j) {
    return j.dump(-1, ' ', false, njson::error_handler_t::replace);
}

// Value → JSON。
njson value_to_json(const Value& v) {
    struct Visitor {
        njson operator()(std::monostate) const { return nullptr; }
        njson operator()(std::int64_t x) const { return x; }
        njson operator()(double x) const { return x; }
        njson operator()(const std::string& s) const { return s; }
    };
    return std::visit(Visitor{}, v);
}

// 公共前置：解析 body → 取 table/datasource → 路由 → 借连接。
// 成功时 guard 有效、datasource 已回传名；失败时返回的 ApiResponse 非空。
struct RoutedPool {
    DbConnectionPool* pool{nullptr};
    std::string datasource;
    // 客户端显式给的 datasource（未给 = 空串）：路由哈希输入的 D1 口径，
    // 与解析后的 datasource 是两个独立量（spec §3.3 期2 修订）。
    std::string explicit_datasource;
};

std::optional<ApiResponse> route_and_prepare(const CrudApiContext& ctx, const njson& doc,
                                             std::string* table, RoutedPool* routed) {
    std::string error;
    if (!require_string(doc, "table", table, &error)) {
        return bad_request(std::move(error));
    }
    std::string datasource;
    bool ds_type_error = false;
    if (!optional_datasource(doc, &datasource, &ds_type_error)) {
        return bad_request("\"datasource\" must be a string");
    }
    const RouteResult route = ctx.registry.route(*table, datasource);
    if (route.status != RouteStatus::kOk) {
        return map_route_error(route);
    }
    routed->pool = route.pool;
    routed->datasource = route.datasource;
    routed->explicit_datasource = datasource;
    return std::nullopt;
}

// ===== 归属门控（cluster-cache spec §3.6 决策矩阵）=====

// 307 而非 302/303：保留 POST 方法与 body，客户端原样重发（决策⑧）。
ApiResponse make_redirect(const std::string& location, const char* path) {
    njson doc;
    doc["code"] = "not_owner";
    doc["message"] = "this node does not own the routing key; retry at Location";
    ApiResponse resp = json_response(307, "Temporary Redirect", safe_dump(doc));
    resp.extra_headers.emplace_back("Location", location + path);
    return resp;
}

struct GateOutcome {
    bool bypass{false};        // 本地处理但 bypass 缓存（规则 A / epoch 门控）
    bool epoch_mismatch{false};
};

// Ownership bypass belongs to one borrowed Database instance, not to the
// request's normal-return path.  The guard restores it before DbConnGuard
// returns the connection to the pool, including exception unwinding.
class CacheBypassGuard {
public:
    CacheBypassGuard(Database& db, bool active) : db_(&db), active_(active) {}
    ~CacheBypassGuard() noexcept {
        if (active_) {
            try {
                db_->set_cache_bypass(false);
            } catch (...) {
                // Reset is cleanup; it must not replace the request's
                // original exception or terminate stack unwinding.
            }
        }
    }

    CacheBypassGuard(const CacheBypassGuard&) = delete;
    CacheBypassGuard& operator=(const CacheBypassGuard&) = delete;

private:
    Database* db_;
    bool active_;
};

// 服务端权威自判：用已借连接查 schema 定路由键 → decide。
// 返回非空 = 直接回 307；否则继续本地执行（门控结果写入 *out）。
// 只对"唯一列且非 REAL"的键门控：非唯一列留给本地校验管道回 400
//（非法请求不踢给 owner）；REAL 键/无键请求不参与环路由（spec §3.6 补充）。
std::optional<ApiResponse> apply_ownership_gate(const CrudApiContext& ctx,
                                                Database& db,
                                                const RoutedPool& routed,
                                                const std::string& table,
                                                const std::string& column,
                                                const Value& value,
                                                const char* path,
                                                GateOutcome* out) {
    if (ctx.router == nullptr) {
        return std::nullopt;
    }
    const db::ColumnInfo* col = db.schema().find_column(table, column);
    // NULL 键值必然被校验管道拒（400）：不门控，避免非法请求被 307
    // 踢给 owner 再拒（与"非法请求不踢给 owner"同口径，不污染 redirects）。
    if (col == nullptr || !col->unique || col->kind == db::ValueKind::kReal ||
        db::is_null(value)) {
        return std::nullopt;
    }
    const OwnershipDecision d = ctx.router->decide(
        routed.explicit_datasource, table, column, value,
        ctx.fallback, ctx.epoch, routed.datasource);
    if (d.action == OwnershipDecision::Action::kRedirect) {
        return make_redirect(d.location, path);
    }
    out->bypass = !d.cache_allowed;
    out->epoch_mismatch = d.epoch_mismatch;
    if (out->bypass) {
        db.set_cache_bypass(true);
    }
    return std::nullopt;
}

// insert 路由键辅助：values 中指定列的值（无则 nullptr）。
const Value* find_column_value(
    const std::vector<std::pair<std::string, Value>>& pairs, const std::string& col) {
    for (const auto& [name, v] : pairs) {
        if (name == col) {
            return &v;
        }
    }
    return nullptr;
}

}  // namespace

ApiResponse handle_select(const CrudApiContext& ctx) {
    ApiResponse error;
    const auto parsed = parse_body_object(ctx, &error);
    if (!parsed.has_value()) {
        return error;
    }
    const njson& doc = *parsed;

    std::string table;
    RoutedPool routed;
    if (const auto early = route_and_prepare(ctx, doc, &table, &routed)) {
        return *early;
    }

    std::string parse_error;
    WhereClause where;
    if (!parse_where(doc, &where, &parse_error)) {
        return bad_request(std::move(parse_error));
    }
    std::vector<std::string> columns;
    const auto cit = doc.find("columns");
    if (cit != doc.end()) {
        if (!cit->is_array()) {
            return bad_request("\"columns\" must be an array of strings");
        }
        for (const auto& item : *cit) {
            if (!item.is_string()) {
                return bad_request("\"columns\" must be an array of strings");
            }
            columns.push_back(item.get<std::string>());
        }
    }

    DbConnGuard guard(*routed.pool, ctx.db_timeout);
    if (!guard.valid()) {
        return make_error_response(503, "Service Unavailable", "db connection timeout");
    }
    GateOutcome gate;
    if (const auto redirect = apply_ownership_gate(ctx, *guard.get(), routed, table,
                                                   where.column, where.value,
                                                   "/api/v1/select", &gate)) {
        return *redirect;
    }
    CacheBypassGuard bypass_guard(*guard.get(), gate.bypass);
    // 统一收口：epoch 回显（§5.4）；bypass 由作用域守卫负责复位。
    auto finalize = [&](ApiResponse r) {
        if (gate.epoch_mismatch) {
            r.extra_headers.emplace_back("X-CppPools-Epoch",
                                         std::to_string(ctx.router->self_epoch()));
        }
        return r;
    };
    const DbResult<ResultSet> result =
        guard.get()->select(SelectRequest{table, std::move(columns), std::move(where)});
    if (result.status != DbStatus::kOk) {
        return finalize(map_db_error(result.status, result.message));
    }

    njson rows = njson::array();
    for (const auto& row : result.value.rows) {
        njson jrow = njson::array();
        for (const auto& cell : row) {
            jrow.push_back(value_to_json(cell));
        }
        rows.push_back(std::move(jrow));
    }
    njson resp;
    resp["datasource"] = routed.datasource;
    resp["columns"] = result.value.columns;
    resp["rows"] = std::move(rows);
    resp["row_count"] = result.value.rows.size();
    return finalize(json_response(200, "OK", safe_dump(resp)));
}

ApiResponse handle_insert(const CrudApiContext& ctx) {
    ApiResponse error;
    const auto parsed = parse_body_object(ctx, &error);
    if (!parsed.has_value()) {
        return error;
    }
    const njson& doc = *parsed;

    std::string table;
    RoutedPool routed;
    if (const auto early = route_and_prepare(ctx, doc, &table, &routed)) {
        return *early;
    }

    const auto vit = doc.find("values");
    if (vit == doc.end() || !vit->is_object()) {
        return bad_request("\"values\" must be an object of column -> value");
    }
    std::string parse_error;
    std::vector<std::pair<std::string, Value>> values;
    if (!parse_column_values(*vit, &values, &parse_error)) {
        return bad_request(std::move(parse_error));
    }

    DbConnGuard guard(*routed.pool, ctx.db_timeout);
    if (!guard.valid()) {
        return make_error_response(503, "Service Unavailable", "db connection timeout");
    }
    GateOutcome gate;
    {
        // 路由键（spec §3.6 补充/D6）：schema 列序第一个"给了值且非 NULL"
        // 的唯一列；一个都没有（纯自增）⇒ 不门控，本地处理。
        const db::TableInfo* tinfo = guard.get()->schema().find_table(table);
        const db::ColumnInfo* key_col = nullptr;
        const Value* key_val = nullptr;
        if (tinfo != nullptr) {
            for (const auto& c : tinfo->columns) {
                if (!c.unique) {
                    continue;
                }
                const Value* v = find_column_value(values, c.name);
                if (v != nullptr && !db::is_null(*v)) {
                    key_col = &c;
                    key_val = v;
                    break;
                }
            }
        }
        if (key_col != nullptr) {
            if (const auto redirect = apply_ownership_gate(
                    ctx, *guard.get(), routed, table, key_col->name, *key_val,
                    "/api/v1/insert", &gate)) {
                return *redirect;
            }
        }
    }
    CacheBypassGuard bypass_guard(*guard.get(), gate.bypass);
    auto finalize = [&](ApiResponse r) {
        if (gate.epoch_mismatch) {
            r.extra_headers.emplace_back("X-CppPools-Epoch",
                                         std::to_string(ctx.router->self_epoch()));
        }
        return r;
    };
    const DbResult<InsertResult> result =
        guard.get()->insert(InsertRequest{table, std::move(values)});
    if (result.status != DbStatus::kOk) {
        return finalize(map_db_error(result.status, result.message));
    }

    njson resp;
    resp["datasource"] = routed.datasource;
    resp["affected_rows"] = result.value.affected_rows;
    resp["inserted_id"] = result.value.inserted_id.has_value()
                              ? njson(*result.value.inserted_id)
                              : njson(nullptr);
    return finalize(json_response(201, "Created", safe_dump(resp)));
}

ApiResponse handle_update(const CrudApiContext& ctx) {
    ApiResponse error;
    const auto parsed = parse_body_object(ctx, &error);
    if (!parsed.has_value()) {
        return error;
    }
    const njson& doc = *parsed;

    std::string table;
    RoutedPool routed;
    if (const auto early = route_and_prepare(ctx, doc, &table, &routed)) {
        return *early;
    }

    const auto sit = doc.find("set");
    if (sit == doc.end() || !sit->is_object()) {
        return bad_request("\"set\" must be an object of column -> value");
    }
    std::string parse_error;
    std::vector<std::pair<std::string, Value>> set;
    if (!parse_column_values(*sit, &set, &parse_error)) {
        return bad_request(std::move(parse_error));
    }
    WhereClause where;
    if (!parse_where(doc, &where, &parse_error)) {
        return bad_request(std::move(parse_error));
    }

    DbConnGuard guard(*routed.pool, ctx.db_timeout);
    if (!guard.valid()) {
        return make_error_response(503, "Service Unavailable", "db connection timeout");
    }
    GateOutcome gate;
    if (const auto redirect = apply_ownership_gate(ctx, *guard.get(), routed, table,
                                                   where.column, where.value,
                                                   "/api/v1/update", &gate)) {
        return *redirect;
    }
    CacheBypassGuard bypass_guard(*guard.get(), gate.bypass);
    auto finalize = [&](ApiResponse r) {
        if (gate.epoch_mismatch) {
            r.extra_headers.emplace_back("X-CppPools-Epoch",
                                         std::to_string(ctx.router->self_epoch()));
        }
        return r;
    };
    const DbResult<RowCount> result =
        guard.get()->update(UpdateRequest{table, std::move(set), std::move(where)});
    if (result.status != DbStatus::kOk) {
        return finalize(map_db_error(result.status, result.message));
    }
    njson resp;
    resp["datasource"] = routed.datasource;
    resp["affected_rows"] = result.value;
    return finalize(json_response(200, "OK", safe_dump(resp)));
}

ApiResponse handle_delete(const CrudApiContext& ctx) {
    ApiResponse error;
    const auto parsed = parse_body_object(ctx, &error);
    if (!parsed.has_value()) {
        return error;
    }
    const njson& doc = *parsed;

    std::string table;
    RoutedPool routed;
    if (const auto early = route_and_prepare(ctx, doc, &table, &routed)) {
        return *early;
    }
    std::string parse_error;
    WhereClause where;
    if (!parse_where(doc, &where, &parse_error)) {
        return bad_request(std::move(parse_error));
    }

    DbConnGuard guard(*routed.pool, ctx.db_timeout);
    if (!guard.valid()) {
        return make_error_response(503, "Service Unavailable", "db connection timeout");
    }
    GateOutcome gate;
    if (const auto redirect = apply_ownership_gate(ctx, *guard.get(), routed, table,
                                                   where.column, where.value,
                                                   "/api/v1/delete", &gate)) {
        return *redirect;
    }
    CacheBypassGuard bypass_guard(*guard.get(), gate.bypass);
    auto finalize = [&](ApiResponse r) {
        if (gate.epoch_mismatch) {
            r.extra_headers.emplace_back("X-CppPools-Epoch",
                                         std::to_string(ctx.router->self_epoch()));
        }
        return r;
    };
    const DbResult<RowCount> result =
        guard.get()->remove(DeleteRequest{table, std::move(where)});
    if (result.status != DbStatus::kOk) {
        return finalize(map_db_error(result.status, result.message));
    }
    njson resp;
    resp["datasource"] = routed.datasource;
    resp["affected_rows"] = result.value;
    return finalize(json_response(200, "OK", safe_dump(resp)));
}

ApiResponse handle_schema(const CrudApiContext& ctx) {
    // 逐源借一条连接读 schema 快照（每源各自独立自省，见 Database 构造）。
    njson sources = njson::array();
    for (const std::string& name : ctx.registry.datasource_names()) {
        DbConnectionPool* pool = ctx.registry.find_pool(name);
        DbConnGuard guard(*pool, ctx.db_timeout);
        if (!guard.valid()) {
            return make_error_response(503, "Service Unavailable",
                                       "db connection timeout (datasource \"" + name + "\")");
        }
        njson ds;
        ds["name"] = name;
        // 刻意不暴露后端类型（SQLite/MySQL）—— 客户端不该依赖的实现细节（spec §7.4）。
        njson tables = njson::array();
        const Database* db = guard.get();
        for (const std::string& tname : db->schema().table_names()) {
            const auto* table = db->schema().find_table(tname);
            njson jtable;
            jtable["name"] = tname;
            njson jcols = njson::array();
            for (const auto& col : table->columns) {
                njson jcol;
                jcol["name"] = col.name;
                jcol["type"] = (col.kind == db::ValueKind::kInteger) ? "integer"
                               : (col.kind == db::ValueKind::kReal)  ? "real"
                                                                     : "text";
                jcol["nullable"] = col.nullable;
                jcol["primary_key"] = col.primary_key;
                jcol["unique"] = col.unique;
                jcols.push_back(std::move(jcol));
            }
            jtable["columns"] = std::move(jcols);
            tables.push_back(std::move(jtable));
        }
        ds["tables"] = std::move(tables);
        sources.push_back(std::move(ds));
    }
    njson resp;
    resp["datasources"] = std::move(sources);
    return json_response(200, "OK", safe_dump(resp));
}

}  // namespace cpppools::server
