#include "server/score_api.h"

#include "cpppools/db/db_connection_pool.h"

#include <cassert>
#include <chrono>
#include <string>

using namespace cpppools::db;
using namespace cpppools::server;


// score_api 四个 CRUD handler 的测试。
//
// 这些 handler 返回纯数据 ApiResponse、完全不接触 socket，
// 因此可以脱离网络直接断言状态码与响应体——这是把 handler 从
// http_server.cpp 里拆出来的主要收益。

namespace {

const std::chrono::milliseconds kTimeout{100};

ScoreApiContext make_ctx(DbConnectionPool& pool, std::string_view query) {
    return ScoreApiContext{pool, query, kTimeout};
}

bool body_contains(const ApiResponse& resp, const std::string& needle) {
    return resp.body.find(needle) != std::string::npos;
}

}  // namespace

static void test_create_returns_201() {
    DbConnectionPool pool(2);
    const ApiResponse resp = handle_create_score(make_ctx(pool, "user=42&score=99"));

    assert(resp.status_code == 201);
    assert(resp.status_text == "Created");
    assert(body_contains(resp, "\"user_id\":42"));
    assert(body_contains(resp, "\"score\":99"));
    assert(body_contains(resp, "\"updated_at_ms\":"));
}

static void test_create_duplicate_returns_409() {
    DbConnectionPool pool(2);
    assert(handle_create_score(make_ctx(pool, "user=1&score=1")).status_code == 201);

    const ApiResponse dup = handle_create_score(make_ctx(pool, "user=1&score=2"));
    assert(dup.status_code == 409);
    assert(dup.status_text == "Conflict");
    assert(body_contains(dup, "\"status_code\":409"));
    assert(body_contains(dup, "\"message\":"));
}

static void test_get_returns_200_then_404() {
    DbConnectionPool pool(2);
    assert(handle_create_score(make_ctx(pool, "user=7&score=55")).status_code == 201);

    const ApiResponse found = handle_get_score(make_ctx(pool, "user=7"));
    assert(found.status_code == 200);
    assert(body_contains(found, "\"score\":55"));

    const ApiResponse missing = handle_get_score(make_ctx(pool, "user=8"));
    assert(missing.status_code == 404);
    assert(missing.status_text == "Not Found");
}

static void test_update_returns_200_then_404() {
    DbConnectionPool pool(2);
    assert(handle_create_score(make_ctx(pool, "user=3&score=10")).status_code == 201);

    const ApiResponse updated = handle_update_score(make_ctx(pool, "user=3&score=20"));
    assert(updated.status_code == 200);
    assert(body_contains(updated, "\"score\":20"));

    const ApiResponse missing = handle_update_score(make_ctx(pool, "user=999&score=1"));
    assert(missing.status_code == 404);
}

static void test_delete_returns_204_then_404() {
    DbConnectionPool pool(2);
    assert(handle_create_score(make_ctx(pool, "user=5&score=1")).status_code == 201);

    const ApiResponse deleted = handle_delete_score(make_ctx(pool, "user=5"));
    assert(deleted.status_code == 204);
    assert(deleted.status_text == "No Content");
    // 204 按 RFC 7230 不得携带响应体。
    assert(deleted.body.empty());

    const ApiResponse missing = handle_delete_score(make_ctx(pool, "user=5"));
    assert(missing.status_code == 404);
}

static void test_invalid_user_returns_400() {
    DbConnectionPool pool(2);

    // 非数字
    assert(handle_get_score(make_ctx(pool, "user=abc")).status_code == 400);
    // 负数
    assert(handle_get_score(make_ctx(pool, "user=-1")).status_code == 400);
    // 存在但为空值——这一条在旧实现里会被误判成「缺少参数」
    const ApiResponse empty_val = handle_get_score(make_ctx(pool, "user="));
    assert(empty_val.status_code == 400);
    assert(body_contains(empty_val, "user"));
}

static void test_invalid_score_returns_400() {
    DbConnectionPool pool(2);
    assert(handle_create_score(make_ctx(pool, "user=1&score=abc")).status_code == 400);
    assert(handle_create_score(make_ctx(pool, "user=1&score=-5")).status_code == 400);
    assert(handle_update_score(make_ctx(pool, "user=1&score=xyz")).status_code == 400);

    // 参数校验必须发生在借连接之前：非法参数不应消耗连接池资源。
    assert(pool.free_count() == 2);
}

static void test_pool_exhausted_returns_503() {
    DbConnectionPool pool(1);
    // 占住唯一一条连接，handler 借不到应回 503 而不是无限阻塞。
    auto* held = pool.acquire(std::chrono::milliseconds(10));
    assert(held != nullptr);

    const ScoreApiContext ctx{pool, "user=1", std::chrono::milliseconds(20)};
    const ApiResponse resp = handle_get_score(ctx);
    assert(resp.status_code == 503);
    assert(resp.status_text == "Service Unavailable");

    pool.release(held);
}

static void test_connection_is_returned_after_each_call() {
    DbConnectionPool pool(2);
    for (int i = 0; i < 50; ++i) {
        handle_create_score(make_ctx(pool, "user=1&score=1"));
        handle_get_score(make_ctx(pool, "user=1"));
        handle_update_score(make_ctx(pool, "user=1&score=2"));
        handle_delete_score(make_ctx(pool, "user=1"));
    }
    // 每次调用都必须把连接归还，否则连续调用会耗尽池子。
    assert(pool.free_count() == 2);
}

static void test_error_body_is_valid_json() {
    DbConnectionPool pool(2);
    const ApiResponse resp = handle_get_score(make_ctx(pool, "user=abc"));
    assert(resp.status_code == 400);
    // 错误体统一 snake_case，且含 message 与 status_code 两个字段。
    assert(body_contains(resp, "\"message\":\""));
    assert(body_contains(resp, "\"status_code\":400"));
    assert(resp.content_type.find("application/json") != std::string::npos);
}

int main() {
    test_create_returns_201();
    test_create_duplicate_returns_409();
    test_get_returns_200_then_404();
    test_update_returns_200_then_404();
    test_delete_returns_204_then_404();
    test_invalid_user_returns_400();
    test_invalid_score_returns_400();
    test_pool_exhausted_returns_503();
    test_connection_is_returned_after_each_call();
    test_error_body_is_valid_json();
    return 0;
}
