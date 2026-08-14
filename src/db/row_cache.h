#pragma once

#include "cpppools/db/value.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cpppools::db {

// 缓存的一行：列名 → 值（整行，投影由消费方现算，与后端口径同构）。
using RowData = std::vector<std::pair<std::string, Value>>;

// 缓存键：(datasource, table, 唯一键列, 键值) 四元组（spec §3.3）。
// 编码带类型前缀：int64 42 与 string "42" 绝不撞键。
struct RowCacheKey {
    std::string datasource;
    std::string table;
    std::string column;
    Value key;

    // 长度前缀编码（与 spec §3.1 同原则，禁止分隔符拼接）。
    std::string encode() const {
        auto lp = [](const std::string& s) {
            std::string out(4, '\0');
            const std::uint32_t len = static_cast<std::uint32_t>(s.size());
            std::memcpy(out.data(), &len, 4);
            out.append(s);
            return out;
        };
        std::string out = lp(datasource) + lp(table) + lp(column);
        if (is_null(key)) {
            out += 'N';
        } else if (const auto* i = std::get_if<std::int64_t>(&key)) {
            out += 'I';
            out.append(reinterpret_cast<const char*>(i), sizeof(*i));
        } else if (const auto* d = std::get_if<double>(&key)) {
            out += 'D';
            out.append(reinterpret_cast<const char*>(d), sizeof(*d));
        } else if (const auto* s = std::get_if<std::string>(&key)) {
            out += 'S';
            out.append(*s);
        }
        return out;
    }
};

struct RowCacheConfig {
    std::size_t max_entries{100000};
    std::size_t max_bytes{static_cast<std::size_t>(256) * 1024 * 1024};
    std::size_t max_entry_bytes{65536};
    std::chrono::milliseconds ttl{60000};
    std::size_t shard_count{16};
};

struct RowCacheStats {
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t evictions{0};
    std::uint64_t invalidations{0};
    std::uint64_t singleflight_merged{0};
    std::uint64_t bypass_too_large{0};
    std::uint64_t backfill_dropped{0};
    std::size_t entries{0};
    std::size_t bytes{0};
};

// 每数据源共享的行级缓存（spec §4.1.1）。
//
// 线程模型：多线程（该数据源的全部池化连接）并发读写，内部 16 分片，
// 每分片一把锁 —— 避免 pool_size 条连接争同一把锁。
//
// 语义要点：
// - 双阈值按分片近似执行：每分片配额 = max / shard_count（下限 1），
//   全局实际容量上限略大于配置值（最多 shard_count 倍的分片内碎片）。
//   生产配置（10 万条 / 16 分片）下碎片可忽略；测试用 shard_count=1 精确验证。
// - TTL 惰性过期：不扫描，get 命中时检查，过期即删（miss）。
// - 失效抬分片世代号：回填方以"回源前记录的世代号"做 CAS，
//   失效后到达的回填直接丢弃（spec §4.3.1，防旧值遮蔽已成功的写）。
// - Singleflight：同键并发 miss 只回源一次，等待者共享结果。
class RowCache {
public:
    // Singleflight 的登机牌：leader 负责回源并 finish_flight；
    // 非 leader 等待 future（其值由 leader 兑现）。
    struct Flight {
        std::shared_ptr<std::shared_future<std::optional<RowData>>> future;
        bool leader{false};
    };

    explicit RowCache(RowCacheConfig cfg)
        : cfg_(std::move(cfg)),
          shards_(shards_count()),
          per_shard_max_entries_(std::max<std::size_t>(1, cfg_.max_entries / shards_.size())),
          per_shard_max_bytes_(std::max<std::size_t>(1, cfg_.max_bytes / shards_.size())) {
        assert(cfg_.shard_count > 0);
    }

    // 读命中（TTL 检查；过期惰性删除）。
    std::optional<RowData> get(const RowCacheKey& key) {
        const std::string k = key.encode();
        Shard& shard = shard_of(k);
        std::lock_guard<std::mutex> lock(shard.mu);
        const auto it = shard.entries.find(k);
        if (it == shard.entries.end()) {
            ++shard.stats.misses;
            return std::nullopt;
        }
        if (expired(shard, it->second)) {
            erase_entry(shard, it);
            ++shard.stats.misses;
            return std::nullopt;
        }
        ++shard.stats.hits;
        touch(shard, it);
        return it->second.row;
    }

    // 回填。超单条上限 → 丢弃并计 bypass；否则入缓存并按双阈值 LRU 淘汰。
    void put(const RowCacheKey& key, RowData row) {
        const std::string k = key.encode();
        Shard& shard = shard_of(k);
        std::lock_guard<std::mutex> lock(shard.mu);
        put_locked(shard, k, std::move(row));
    }

    // 失效：删条目 + 分片世代号 +1（无条件，见 spec 不变量 I0）。
    void invalidate(const RowCacheKey& key) {
        const std::string k = key.encode();
        Shard& shard = shard_of(k);
        std::lock_guard<std::mutex> lock(shard.mu);
        const auto it = shard.entries.find(k);
        if (it != shard.entries.end()) {
            erase_entry(shard, it);
        }
        ++shard.generation;
        ++shard.stats.invalidations;
    }

    // 当前分片世代号（回填 CAS 用：回源前记录，回填时比对）。
    std::uint64_t generation(const RowCacheKey& key) {
        const std::string k = key.encode();
        Shard& shard = shard_of(k);
        std::lock_guard<std::mutex> lock(shard.mu);
        return shard.generation;
    }

    // Singleflight：同键并发 miss 合并为一次回源。
    // 键已在缓存 → 返回就绪 future + leader=false（不产生新 flight）。
    Flight join_flight(const RowCacheKey& key) {
        const std::string k = key.encode();
        Shard& shard = shard_of(k);
        std::lock_guard<std::mutex> lock(shard.mu);
        const auto cached = shard.entries.find(k);
        if (cached != shard.entries.end() && !expired(shard, cached->second)) {
            touch(shard, cached);
            auto ready = std::make_shared<std::promise<std::optional<RowData>>>();
            ready->set_value(cached->second.row);
            ++shard.stats.hits;
            Flight f;
            f.future = std::make_shared<std::shared_future<std::optional<RowData>>>(
                ready->get_future().share());
            f.leader = false;
            return f;
        }
        const auto fit = shard.flights.find(k);
        if (fit != shard.flights.end()) {
            ++shard.stats.singleflight_merged;
            Flight f;
            f.future = fit->second.second;
            f.leader = false;
            return f;
        }
        auto promise = std::make_shared<std::promise<std::optional<RowData>>>();
        auto shared = std::make_shared<std::shared_future<std::optional<RowData>>>(
            promise->get_future().share());
        shard.flights.emplace(k, std::make_pair(std::move(promise), shared));
        ++shard.stats.misses;
        Flight f;
        f.future = shared;
        f.leader = true;
        return f;
    }

    // leader 完成回源：同一临界区内原子完成「摘 flight + 世代比对 + 插入条目」，
    // 锁外兑现 future。世代比对与写入必须原子：若分两步（锁外检查 +
    // 锁内写），invalidate 可在两步之间插入，旧值照样写进缓存 ——
    // 正是 spec §4.3.1 要防的"旧值遮蔽已成功的写"。
    // 世代不匹配 → 丢弃回填并计 backfill_dropped；返回是否实际入缓存。
    bool finish_flight(const RowCacheKey& key, std::optional<RowData> result,
                       std::uint64_t expected_generation) {
        const std::string k = key.encode();
        Shard& shard = shard_of(k);
        std::shared_ptr<std::promise<std::optional<RowData>>> promise;
        bool stored = false;
        {
            std::lock_guard<std::mutex> lock(shard.mu);
            const auto fit = shard.flights.find(k);
            if (fit != shard.flights.end()) {
                promise = fit->second.first;
                shard.flights.erase(fit);
            }
            if (result.has_value()) {
                if (shard.generation == expected_generation) {
                    stored = put_locked(shard, k, *result);
                } else {
                    ++shard.stats.backfill_dropped;
                }
            }
        }
        if (promise) {
            promise->set_value(std::move(result));
        }
        return stored;
    }

    RowCacheStats stats() const {
        RowCacheStats total;
        for (const Shard& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mu);
            total.hits += shard.stats.hits;
            total.misses += shard.stats.misses;
            total.evictions += shard.stats.evictions;
            total.invalidations += shard.stats.invalidations;
            total.singleflight_merged += shard.stats.singleflight_merged;
            total.bypass_too_large += shard.stats.bypass_too_large;
            total.backfill_dropped += shard.stats.backfill_dropped;
            total.entries += shard.entries.size();
            total.bytes += shard.bytes;
        }
        return total;
    }

    // 运行时开关（kill-switch，spec §8.3）：关闭后装饰器整体直通内层。
    // const：指标/控制路径经 const 引用访问；原子写本身是线程安全的。
    void set_enabled(bool enabled) const { enabled_.store(enabled, std::memory_order_relaxed); }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

private:
    struct Entry {
        RowData row;
        std::chrono::steady_clock::time_point cached_at;
        std::size_t bytes;
        std::list<std::string>::iterator lru_iter;
    };

    struct ShardStats {
        std::uint64_t hits{0};
        std::uint64_t misses{0};
        std::uint64_t evictions{0};
        std::uint64_t invalidations{0};
        std::uint64_t singleflight_merged{0};
        std::uint64_t bypass_too_large{0};
        std::uint64_t backfill_dropped{0};
    };

    struct Shard {
        mutable std::mutex mu;
        std::unordered_map<std::string, Entry> entries;
        std::list<std::string> lru;  // 头部 = 最近使用
        std::size_t bytes{0};
        std::uint64_t generation{0};
        // 键 → (promise, shared_future)：leader 兑现，等待者共享。
        std::unordered_map<
            std::string,
            std::pair<std::shared_ptr<std::promise<std::optional<RowData>>>,
                      std::shared_ptr<std::shared_future<std::optional<RowData>>>>>
            flights;
        ShardStats stats;
    };

    static std::size_t row_bytes(const RowData& row) {
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

    std::size_t shards_count() const {
        return cfg_.shard_count > 0 ? cfg_.shard_count : 1;
    }

    Shard& shard_of(const std::string& k) {
        // FNV-1a：分片只需要分布均匀，与两端一致性口径无关。
        std::uint64_t h = 1469598103934665603ull;
        for (const char c : k) {
            h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
            h *= 1099511628211ull;
        }
        return shards_[h % shards_.size()];
    }

    bool expired(const Shard&, const Entry& entry) const {
        return std::chrono::steady_clock::now() - entry.cached_at >= cfg_.ttl;
    }

    void touch(Shard& shard,
               const std::unordered_map<std::string, Entry>::iterator& it) {
        shard.lru.splice(shard.lru.begin(), shard.lru, it->second.lru_iter);
    }

    void erase_entry(Shard& shard,
                     const std::unordered_map<std::string, Entry>::iterator& it) {
        shard.bytes -= it->second.bytes;
        shard.lru.erase(it->second.lru_iter);
        shard.entries.erase(it);
    }

    void evict_until_fit(Shard& shard) {
        while ((shard.entries.size() > per_shard_max_entries_ ||
                shard.bytes > per_shard_max_bytes_) &&
               !shard.lru.empty()) {
            const auto lru_it = std::prev(shard.lru.end());
            const auto it = shard.entries.find(*lru_it);
            if (it != shard.entries.end()) {
                erase_entry(shard, it);
                ++shard.stats.evictions;
            } else {
                shard.lru.erase(lru_it);
            }
        }
    }

    // put 的锁内主体（调用方持有 shard.mu）。返回是否实际入缓存。
    bool put_locked(Shard& shard, const std::string& k, RowData row) {
        const std::size_t bytes = row_bytes(row);
        if (bytes > cfg_.max_entry_bytes) {
            ++shard.stats.bypass_too_large;
            return false;
        }
        const auto it = shard.entries.find(k);
        if (it != shard.entries.end()) {
            erase_entry(shard, it);
        }
        shard.lru.push_front(k);
        shard.entries.emplace(k, Entry{std::move(row), std::chrono::steady_clock::now(),
                                       bytes, shard.lru.begin()});
        shard.bytes += bytes;
        evict_until_fit(shard);
        return true;
    }

    RowCacheConfig cfg_;
    std::vector<Shard> shards_;
    const std::size_t per_shard_max_entries_;
    const std::size_t per_shard_max_bytes_;
    mutable std::atomic<bool> enabled_{true};
};

}  // namespace cpppools::db
