#include "db_pool/db_connection_pool.h"
#include "memory_pool/object_pool.h"

#include <cassert>
#include <chrono>
#include <string>

struct Dummy {
    int x{0};
    std::string name;
};

static void test_object_pool_basic() {
    ObjectPool<Dummy> pool(2);
    auto a = pool.acquire();
    auto b = pool.acquire();
    assert(pool.in_use() == 2);
    assert(pool.total_size() >= 2);

    a->x = 7;
    b->name = "ok";
}

static void test_object_pool_raii_return() {
    ObjectPool<Dummy> pool(1);
    {
        auto obj = pool.acquire();
        obj->x = 42;
        assert(pool.in_use() == 1);
        assert(pool.available() == 0);
    }
    // obj goes out of scope and should automatically return to pool.
    assert(pool.in_use() == 0);
    assert(pool.available() == 1);
}

static void test_db_pool_basic() {
    DBConnectionPool pool(2);
    auto* a = pool.acquire(std::chrono::milliseconds(10));
    auto* b = pool.acquire(std::chrono::milliseconds(10));
    assert(a != nullptr);
    assert(b != nullptr);
    assert(pool.free_count() == 0);

    pool.release(a);
    pool.release(b);
    assert(pool.free_count() == 2);
}

static void test_db_pool_timeout() {
    DBConnectionPool pool(1);
    auto* held = pool.acquire(std::chrono::milliseconds(10));
    assert(held != nullptr);

    // Pool is exhausted here, so second acquire should timeout and return nullptr.
    auto* timed_out = pool.acquire(std::chrono::milliseconds(30));
    assert(timed_out == nullptr);

    pool.release(held);
    assert(pool.free_count() == 1);
}

static void test_db_conn_guard_auto_release() {
    DBConnectionPool pool(1);
    {
        DBConnGuard guard(pool, std::chrono::milliseconds(10));
        assert(guard.valid());
        assert(pool.free_count() == 0);
    }
    // Guard dtor should release the borrowed connection.
    assert(pool.free_count() == 1);
}

static void test_db_pool_double_release_is_ignored() {
    DBConnectionPool pool(1);
    auto* conn = pool.acquire(std::chrono::milliseconds(10));
    assert(conn != nullptr);

    pool.release(conn);
    pool.release(conn);  // duplicated release should be ignored
    assert(pool.free_count() == 1);

    auto* one = pool.acquire(std::chrono::milliseconds(10));
    assert(one != nullptr);
    auto* two = pool.acquire(std::chrono::milliseconds(20));
    assert(two == nullptr);
    pool.release(one);
}

int main() {
    test_object_pool_basic();
    test_object_pool_raii_return();
    test_db_pool_basic();
    test_db_pool_timeout();
    test_db_conn_guard_auto_release();
    test_db_pool_double_release_is_ignored();
    return 0;
}
