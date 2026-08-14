// RowCache 单元测试（cluster-cache spec §4.1.1/§4.2/§4.3.1）。
//
// RowCache 是每数据源共享的行级缓存：LRU 双阈值（条目数 + 字节数）、
// TTL 惰性过期、失效世代号（回填 CAS）、Singleflight 合并。

#include "db/row_cache.h"

#include <cassert>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using cpppools::db::RowCache;
using cpppools::db::RowCacheConfig;
using cpppools::db::RowCacheKey;
using cpppools::db::RowData;
using cpppools::db::Value;

namespace {

RowCacheKey key_of(const std::string& table, std::int64_t id) {
    return RowCacheKey{"main", table, "id", Value{id}};
}

RowData make_row(std::int64_t id, const std::string& name) {
    return RowData{{"id", Value{id}}, {"name", Value{name}}};
}

std::size_t row_bytes(const RowData& row) {
    std::size_t b = 0;
    for (const auto& [col, v] : row) {
        b += col.size();
        if (const auto* s = std::get_if<std::string>(&v)) {
            b += s->size();
        } else {
            b += 8;
        }
    }
    return b;
}

RowCacheConfig fast_ttl_config() {
    RowCacheConfig cfg;
    cfg.max_entries = 8;
    cfg.max_bytes = 1u << 20;
    cfg.max_entry_bytes = 4096;
    cfg.ttl = std::chrono::milliseconds(60);
    return cfg;
}

}  // namespace

// ---------------------------------------------------------------------------
// 基本读写
// ---------------------------------------------------------------------------

static void test_put_get_roundtrip() {
    RowCache cache(fast_ttl_config());
    const auto key = key_of("users", 1);
    const RowData row = make_row(1, "alice");

    assert(!cache.get(key).has_value());
    cache.put(key, row);
    const auto got = cache.get(key);
    assert(got.has_value());
    assert(got->size() == 2);
    assert((*got)[0].first == "id");
    assert(std::get<std::int64_t>((*got)[0].second) == 1);
    assert(std::get<std::string>((*got)[1].second) == "alice");

    const auto stats = cache.stats();
    assert(stats.hits == 1);
    assert(stats.misses == 1);
    assert(stats.entries == 1);
    assert(stats.bytes == row_bytes(row));
}

static void test_different_keys_do_not_collide() {
    RowCache cache(fast_ttl_config());
    // 同值不同类型（int64 42 vs string "42"）绝不能撞键：键编码带类型前缀。
    const RowCacheKey int_key{"main", "users", "id", Value{std::int64_t{42}}};
    const RowCacheKey str_key{"main", "users", "id", Value{std::string("42")}};
    cache.put(int_key, make_row(42, "int-row"));
    assert(cache.get(int_key).has_value());
    assert(!cache.get(str_key).has_value());

    // 不同 datasource 的同名表同键也隔离。
    const RowCacheKey other_ds{"archive", "users", "id", Value{std::int64_t{42}}};
    assert(!cache.get(other_ds).has_value());
}

// ---------------------------------------------------------------------------
// TTL 惰性过期
// ---------------------------------------------------------------------------

static void test_ttl_expiry() {
    auto cfg = fast_ttl_config();
    cfg.ttl = std::chrono::milliseconds(30);
    RowCache cache(cfg);
    const auto key = key_of("users", 1);
    cache.put(key, make_row(1, "alice"));
    assert(cache.get(key).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(!cache.get(key).has_value() && "过期条目必须惰性删除");
    assert(cache.stats().entries == 0);
}

// ---------------------------------------------------------------------------
// LRU 淘汰：条目数阈值 + 字节数阈值
// ---------------------------------------------------------------------------

static void test_lru_evicts_by_entry_count() {
    auto cfg = fast_ttl_config();
    cfg.max_entries = 3;
    cfg.shard_count = 1;  // 单分片：阈值精确（多分片下是按分片近似）
    RowCache cache(cfg);
    for (std::int64_t i = 1; i <= 4; ++i) {
        cache.put(key_of("users", i), make_row(i, "x"));
    }
    assert(cache.stats().entries == 3);
    assert(!cache.get(key_of("users", 1)).has_value() && "最老条目必须被淘汰");
    assert(cache.get(key_of("users", 4)).has_value());
    assert(cache.stats().evictions == 1);
}

static void test_lru_evicts_by_bytes() {
    auto cfg = fast_ttl_config();
    cfg.max_bytes = 200;
    cfg.max_entry_bytes = 1 << 20;
    cfg.shard_count = 1;
    RowCache cache(cfg);
    // 两条 ~100 字节条目：第二条触发第一条淘汰。
    const RowData fat = make_row(1, std::string(90, 'a'));
    cache.put(key_of("users", 1), fat);
    cache.put(key_of("users", 2), fat);
    assert(cache.stats().entries == 1);
    assert(cache.stats().bytes <= 200);
    assert(!cache.get(key_of("users", 1)).has_value());
    assert(cache.get(key_of("users", 2)).has_value());
}

static void test_oversized_entry_not_cached() {
    auto cfg = fast_ttl_config();
    cfg.max_entry_bytes = 32;
    RowCache cache(cfg);
    const RowData fat = make_row(1, std::string(100, 'a'));
    cache.put(key_of("users", 1), fat);
    assert(cache.stats().entries == 0 && "超过单条上限的条目不得入缓存");
    assert(cache.stats().bypass_too_large == 1);
}

// ---------------------------------------------------------------------------
// 失效与世代号（回填 CAS 的地基，spec §4.3.1）
// ---------------------------------------------------------------------------

static void test_invalidate_removes_and_bumps_generation() {
    RowCache cache(fast_ttl_config());
    const auto key = key_of("users", 1);
    cache.put(key, make_row(1, "alice"));
    const std::uint64_t gen0 = cache.generation(key);
    cache.invalidate(key);
    assert(!cache.get(key).has_value());
    assert(cache.generation(key) == gen0 + 1);
    assert(cache.stats().invalidations == 1);
    // 对不存在的键失效也要抬世代号（写穿透的失效不依赖条目存在）。
    const std::uint64_t gen1 = cache.generation(key);
    cache.invalidate(key);
    assert(cache.generation(key) == gen1 + 1);
}

static void test_singleflight_only_one_leader() {
    RowCache cache(fast_ttl_config());
    const auto key = key_of("users", 1);

    // 并发 join 同一键：恰有一个 leader，其余拿同一 future。
    constexpr int kWaiters = 8;
    std::vector<RowCache::Flight> flights(kWaiters);
    std::vector<std::thread> threads;
    std::mutex mu;
    std::vector<std::size_t> leader_slots;
    for (std::size_t i = 0; i < kWaiters; ++i) {
        threads.emplace_back([&, i] {
            RowCache::Flight f = cache.join_flight(key);
            std::lock_guard<std::mutex> lock(mu);
            flights[i] = std::move(f);
            if (flights[i].leader) {
                leader_slots.push_back(i);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    assert(leader_slots.size() == 1 && "同键并发必须只有一个 leader");

    // leader 完成回填 → 等待者拿到同一结果。
    const std::uint64_t gen0 = cache.generation(key);
    cache.finish_flight(key, make_row(1, "alice"), gen0);
    for (std::size_t i = 0; i < kWaiters; ++i) {
        const auto result = flights[i].future->get();
        assert(result.has_value());
        assert(std::get<std::string>((*result)[1].second) == "alice");
    }
    assert(cache.stats().singleflight_merged >= kWaiters - 1);
    // 回填落地为缓存条目。
    assert(cache.get(key).has_value());
}

static void test_singleflight_releases_key_after_finish() {
    RowCache cache(fast_ttl_config());
    const auto key = key_of("users", 1);
    cache.finish_flight(key, make_row(1, "a"), cache.generation(key));
    // flight 结束后再 join：条目已在缓存 —— join_flight 对已有条目的键
    // 直接给 leader=false + 就绪 future（不产生新 flight）。
    RowCache::Flight f = cache.join_flight(key);
    assert(!f.leader);
    assert(f.future->wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    assert(f.future->get().has_value());
}

// 世代号拦下回填（spec §4.3.1 / §9）：不用 sleep 赌竞态 —— 时序完全由
// API 调用顺序构造：记录 seq0 → 失效（抬世代）→ 以旧 seq0 回填必被丢弃。
static void test_backfill_after_invalidation_is_dropped() {
    RowCache cache(fast_ttl_config());
    const auto key = key_of("users", 1);

    // leader 回源前记录世代。
    const std::uint64_t seq0 = cache.generation(key);
    // 回源期间发生写：invalidate 抬世代。
    cache.invalidate(key);
    // 旧世代回填到达：原子 CAS 拦下，不入缓存且计入 dropped。
    const bool stored = cache.finish_flight(key, make_row(1, "stale"), seq0);
    assert(!stored);
    assert(!cache.get(key).has_value() && "失效后的陈旧回填不得入缓存");
    assert(cache.stats().backfill_dropped == 1);

    // 新世代回填正常入缓存，dropped 不再增长。
    const std::uint64_t seq1 = cache.generation(key);
    assert(cache.finish_flight(key, make_row(1, "fresh"), seq1));
    assert(cache.get(key).has_value());
    assert(cache.stats().backfill_dropped == 1);
}

// 全量清空（期4 归队 Rejoin，spec §5.2 规则 B）：条目/字节归零，
// flush 计数递增；clear 自身不节流（节流在服务端层，§5.2 的 10s 窗口）。
static void test_clear_removes_all_and_counts_flush() {
    RowCache cache(fast_ttl_config());
    cache.put(key_of("users", 1), make_row(1, "a"));
    cache.put(key_of("users", 2), make_row(2, "b"));
    assert(cache.stats().entries == 2);

    cache.clear();
    assert(cache.stats().entries == 0);
    assert(cache.stats().bytes == 0);
    assert(!cache.get(key_of("users", 1)).has_value());
    assert(!cache.get(key_of("users", 2)).has_value());
    assert(cache.stats().flushes == 1);

    // 幂等：空缓存再 clear 也计数（节流职责不在这里）。
    cache.clear();
    assert(cache.stats().flushes == 2);
    // 清空后可正常重新回填。
    cache.put(key_of("users", 3), make_row(3, "c"));
    assert(cache.get(key_of("users", 3)).has_value());
}

int main() {
    test_put_get_roundtrip();
    test_different_keys_do_not_collide();
    test_ttl_expiry();
    test_lru_evicts_by_entry_count();
    test_lru_evicts_by_bytes();
    test_oversized_entry_not_cached();
    test_invalidate_removes_and_bumps_generation();
    test_singleflight_only_one_leader();
    test_singleflight_releases_key_after_finish();
    test_backfill_after_invalidation_is_dropped();
    test_clear_removes_all_and_counts_flush();
    return 0;
}
