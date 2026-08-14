#pragma once

#include "cpppools/db/score_record.h"

#include <cstddef>
#include <shared_mutex>
#include <unordered_map>

namespace cpppools::db {

// MockScoreStore：mock 模式下的进程级共享存储。
//
// 为什么必须存在：
//   mock 模式原本用纯函数 (user_id * 17 + conn_id * 3) % 101 生成分数，完全无状态。
//   但 CRUD 语义要求「用连接 3 写入、用连接 7 能读到」，
//   而连接池里每个 DbConnection 都是独立对象，
//   因此需要一份被所有 mock 连接共享的存储来承载状态。
//
// 生命周期：由**调用方**持有，构造时以非拥有指针注入各 mock 连接。
//   factory 化后池不再持有 store（spec §6.1.2）：生产路径由 main.cpp 提供；
//   池的便利构造（仅按大小/配置构造）内部用一个进程级静态单例
//   （见 db_connection_pool.cpp 的 mock_store()）。
//   需要隔离的测试（如 test_db_connection.cpp）自己构造独立实例经 factory 注入。
//
// 并发策略：get 远多于写，故使用 std::shared_mutex（C++17）：
//   读路径用 shared_lock 并发进入，写路径用 unique_lock 独占。
//   这是相对 std::mutex 的直接收益，也与项目中「stats getter 要无锁读」的既有考量一致。
class MockScoreStore {
public:
    MockScoreStore() = default;

    MockScoreStore(const MockScoreStore&) = delete;
    MockScoreStore& operator=(const MockScoreStore&) = delete;

    // 不存在则插入并返回新记录；已存在则返回 kAlreadyExists 且不修改原值。
    ScoreResult create(int user_id, int score);

    // 存在则返回记录快照；否则返回 kNotFound。
    ScoreResult get(int user_id) const;

    // 存在则覆盖 score 并刷新 updated_at_ms；否则返回 kNotFound 且不插入。
    ScoreResult update(int user_id, int score);

    // 存在则删除并返回被删记录的快照；否则返回 kNotFound。
    ScoreResult remove(int user_id);

    std::size_t size() const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<int, ScoreRecord> records_;
};

}  // namespace cpppools::db
