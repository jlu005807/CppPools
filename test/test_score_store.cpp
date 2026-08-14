#include "cpppools/db/mock_score_store.h"

#include <cassert>
#include <thread>
#include <vector>

using namespace cpppools::db;


// MockScoreStore 的 CRUD 语义测试。
//
// 这一层是 mock 模式下 CRUD 的唯一状态来源，因此重点验证四件事：
// 1) 每个操作在「命中」与「未命中」两条路径上返回正确的 DbOpStatus；
// 2) create 不覆盖已有记录（幂等安全）；
// 3) update / remove 对不存在的 key 不产生副作用；
// 4) 并发写入不丢记录（shared_mutex 的正确性）。

static void test_create_then_get() {
    MockScoreStore store;

    const ScoreResult created = store.create(42, 99);
    assert(created.status == DbOpStatus::kOk);
    assert(created.record.user_id == 42);
    assert(created.record.score == 99);
    assert(created.record.updated_at_ms > 0);

    const ScoreResult got = store.get(42);
    assert(got.status == DbOpStatus::kOk);
    assert(got.record.score == 99);
    assert(store.size() == 1);
}

static void test_create_duplicate_is_rejected() {
    MockScoreStore store;
    assert(store.create(7, 10).status == DbOpStatus::kOk);

    const ScoreResult again = store.create(7, 20);
    assert(again.status == DbOpStatus::kAlreadyExists);

    // 原值必须保持不变：create 绝不能退化成 upsert。
    assert(store.get(7).record.score == 10);
    assert(store.size() == 1);
}

static void test_get_missing_returns_not_found() {
    MockScoreStore store;
    const ScoreResult got = store.get(12345);
    assert(got.status == DbOpStatus::kNotFound);
    assert(!got.ok());
}

static void test_update_existing() {
    MockScoreStore store;
    assert(store.create(1, 50).status == DbOpStatus::kOk);

    const ScoreResult updated = store.update(1, 88);
    assert(updated.status == DbOpStatus::kOk);
    assert(updated.record.score == 88);
    assert(store.get(1).record.score == 88);
    assert(store.size() == 1);
}

static void test_update_missing_returns_not_found() {
    MockScoreStore store;
    const ScoreResult updated = store.update(999, 1);
    assert(updated.status == DbOpStatus::kNotFound);
    // 不得因 update 失败而插入新记录。
    assert(store.size() == 0);
}

static void test_update_to_same_value_still_ok() {
    // 这条对应 MySQL 分支的已知陷阱：affected_rows 在「值未改变」时为 0。
    // mock 分支不该有这个问题，用测试把语义钉住。
    MockScoreStore store;
    assert(store.create(5, 60).status == DbOpStatus::kOk);
    assert(store.update(5, 60).status == DbOpStatus::kOk);
}

static void test_remove_existing() {
    MockScoreStore store;
    assert(store.create(3, 30).status == DbOpStatus::kOk);

    const ScoreResult removed = store.remove(3);
    assert(removed.status == DbOpStatus::kOk);
    assert(removed.record.score == 30);  // 返回被删除的那份快照
    assert(store.size() == 0);
    assert(store.get(3).status == DbOpStatus::kNotFound);
}

static void test_remove_missing_returns_not_found() {
    MockScoreStore store;
    assert(store.remove(404).status == DbOpStatus::kNotFound);
    assert(store.size() == 0);
}

static void test_concurrent_creates_do_not_lose_records() {
    MockScoreStore store;
    const int kThreads = 8;
    const int kPerThread = 100;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&store, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                store.create(t * kPerThread + i, i);
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }

    assert(store.size() == static_cast<std::size_t>(kThreads * kPerThread));
}

int main() {
    test_create_then_get();
    test_create_duplicate_is_rejected();
    test_get_missing_returns_not_found();
    test_update_existing();
    test_update_missing_returns_not_found();
    test_update_to_same_value_still_ok();
    test_remove_existing();
    test_remove_missing_returns_not_found();
    test_concurrent_creates_do_not_lose_records();
    return 0;
}
