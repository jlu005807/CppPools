#include "cpppools/db/db_connection.h"
#include "cpppools/db/db_connection_pool.h"
#include "cpppools/db/mock_score_store.h"

#include <cassert>
#include <chrono>
#include <memory>

using namespace cpppools::db;
using namespace std::chrono_literals;

// 这些用例自 test_pools.cpp 迁入（批 3）。它们验证的是 mock 后端的
// CRUD 语义与跨连接可见性，不属于「池」本身的行为（那部分在 test_db_pool.cpp）。
//
// 共享存储按 spec §6.1.2 的语义显式构造：**同一后端 = 同一份数据**。
// factory 化之前它是池的成员（一池一份）；现在由调用方提供，
// 生产里 main.cpp 只有一个存储，因此这里也用一个文件级共享存储。
namespace {
MockScoreStore g_store;

ConnectionFactory make_store_factory() {
    return [](std::size_t slot_index) -> std::unique_ptr<DbConnection> {
        return std::make_unique<DbConnection>(static_cast<int>(slot_index) + 1, &g_store);
    };
}
}  // namespace

// CRUD 写入必须跨连接可见。
//
// 这是共享存储存在的根本理由：池里每个 DbConnection 都是独立对象，
// 如果状态存在连接自身里，用连接 A 写入后用连接 B 就读不到，
// 而客户端无法控制自己拿到哪条连接。
static void test_crud_state_is_shared_across_connections() {
    DbConnectionPool pool("crud-shared", make_store_factory(), 4);

    auto* writer = pool.acquire(10ms);
    auto* reader = pool.acquire(10ms);
    assert(writer != nullptr);
    assert(reader != nullptr);
    assert(writer != reader);  // 确保确实是两条不同的连接

    assert(writer->create_score(77, 42).status == DbOpStatus::kOk);

    const ScoreResult got = reader->get_score(77);
    assert(got.status == DbOpStatus::kOk);
    assert(got.record.user_id == 77);
    assert(got.record.score == 42);

    // 反向验证：reader 改、writer 读。
    assert(reader->update_score(77, 43).status == DbOpStatus::kOk);
    assert(writer->get_score(77).record.score == 43);

    assert(reader->delete_score(77).status == DbOpStatus::kOk);
    assert(writer->get_score(77).status == DbOpStatus::kNotFound);

    pool.release(writer);
    pool.release(reader);
}

// 两个指向**不同后端**的池不得共享状态。
//
// 注意与旧版用例的区别：factory 化之前两个 mock 池各持一份存储，
// 「隔离」是自动的；现在隔离与否取决于 factory 指向的存储 ——
// 这正是 spec §6.1.2「store 归属调用方」的直接体现。
static void test_crud_state_is_isolated_between_stores() {
    MockScoreStore store_a;
    MockScoreStore store_b;
    auto factory_a = [&store_a](std::size_t i) {
        return std::make_unique<DbConnection>(static_cast<int>(i) + 1, &store_a);
    };
    auto factory_b = [&store_b](std::size_t i) {
        return std::make_unique<DbConnection>(static_cast<int>(i) + 1, &store_b);
    };

    DbConnectionPool pool_a("iso-a", factory_a, 1);
    DbConnectionPool pool_b("iso-b", factory_b, 1);

    auto* a = pool_a.acquire(10ms);
    auto* b = pool_b.acquire(10ms);
    assert(a->create_score(500, 1).status == DbOpStatus::kOk);
    assert(b->get_score(500).status == DbOpStatus::kNotFound);

    pool_a.release(a);
    pool_b.release(b);
}

static void test_crud_failure_paths() {
    DbConnectionPool pool("crud-fail", make_store_factory(), 1);
    auto* conn = pool.acquire(10ms);
    assert(conn != nullptr);

    assert(conn->get_score(9001).status == DbOpStatus::kNotFound);
    assert(conn->update_score(9001, 5).status == DbOpStatus::kNotFound);
    assert(conn->delete_score(9001).status == DbOpStatus::kNotFound);

    assert(conn->create_score(9001, 5).status == DbOpStatus::kOk);
    assert(conn->create_score(9001, 6).status == DbOpStatus::kAlreadyExists);
    assert(conn->get_score(9001).record.score == 5);  // 重复创建不得覆盖

    pool.release(conn);
}

int main() {
    test_crud_state_is_shared_across_connections();
    test_crud_state_is_isolated_between_stores();
    test_crud_failure_paths();
    return 0;
}
