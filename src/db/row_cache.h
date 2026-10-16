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

#ifdef CPPPOOLS_USE_REDIS
#include "cache/redis_row_store.h"
#endif

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
#ifdef CPPPOOLS_USE_REDIS
    std::shared_ptr<cache::RedisRowStore> redis_store;
#endif
};

struct RowCacheStats {
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t evictions{0};
    std::uint64_t invalidations{0};
    std::uint64_t singleflight_merged{0};
    std::uint64_t bypass_too_large{0};
    std::uint64_t backfill_dropped{0};
    std::uint64_t flushes{0};
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
        std::uint64_t flight_id{0};
        std::uint64_t generation{0};
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
#ifdef CPPPOOLS_USE_REDIS
        if (cfg_.redis_store && cfg_.redis_store->available()) {
            const auto remote = cfg_.redis_store->get(redis_key(k));
            if (remote.code == cache::RedisStoreCode::hit) {
                RowData row;
                if (decode_row(remote.value, &row)) {
                    Shard& shard = shard_of(k);
                    std::lock_guard<std::mutex> lock(shard.mu);
                    ++shard.stats.hits;
                    return row;
                }
                cfg_.redis_store->disable();
            } else if (remote.code == cache::RedisStoreCode::unavailable) {
                // The store is retired by RedisRowStore; use cold memory below.
            }
        }
#endif
        Shard& shard = shard_of(k);
        std::lock_guard<std::mutex> lock(shard.mu);
        const auto it = shard.entries.find(k);
        if (it == shard.entries.end()) {
            return std::nullopt;
        }
        if (expired(shard, it->second)) {
            erase_entry(shard, it);
            return std::nullopt;
        }
        ++shard.stats.hits;
        touch(shard, it);
        return it->second.row;
    }

    // 回填。超单条上限 → 丢弃并计 bypass；否则入缓存并按双阈值 LRU 淘汰。
    void put(const RowCacheKey& key, RowData row) {
        const std::string k = key.encode();
#ifdef CPPPOOLS_USE_REDIS
        if (cfg_.redis_store && cfg_.redis_store->available()) {
            if (row_bytes(row) <= cfg_.max_entry_bytes &&
                cfg_.redis_store->put(redis_key(k), encode_row(row), cfg_.ttl)) return;
        }
#endif
        Shard& shard = shard_of(k);
        std::lock_guard<std::mutex> lock(shard.mu);
        put_locked(shard, k, std::move(row), std::chrono::steady_clock::now());
    }

    // 失效：删条目 + 分片世代号 +1（无条件，见 spec 不变量 I0）。
    void invalidate(const RowCacheKey& key) {
        const std::string k = key.encode();
        Shard& shard = shard_of(k);
        std::shared_ptr<std::promise<std::optional<RowData>>> retired;
        {
            std::lock_guard<std::mutex> lock(shard.mu);
            const auto it = shard.entries.find(k);
            if (it != shard.entries.end()) {
                erase_entry(shard, it);
            }
            const auto fit = shard.flights.find(k);
            if (fit != shard.flights.end()) {
                retired = fit->second.promise;
                shard.flights.erase(fit);
            }
            ++shard.generation;
            ++shard.stats.invalidations;
        }
        if (retired) {
            retired->set_value(std::nullopt);
        }
#ifdef CPPPOOLS_USE_REDIS
        if (cfg_.redis_store && cfg_.redis_store->available() &&
            !cfg_.redis_store->erase(redis_key(k))) {
            // RedisRowStore retires itself; the next read uses cold memory.
        }
#endif
    }

    // 全量清空（归队 Rejoin，spec §5.2 规则 B：best-effort 优化）。
    // 只清条目，不抬世代号：规则 B 的语义是"丢弃旧条目加速收敛"，
    // 不是失效 —— 在途回填仍有世代 CAS 兜底，不改变一致性契约。
    // 节流（10s 窗口）由服务端层负责，本方法自身幂等可重入。
    // const：与 set_enabled 同模式（registry 以 const 指针暴露缓存）。
    void clear() const {
        std::vector<std::shared_ptr<std::promise<std::optional<RowData>>>> retired;
        for (const Shard& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mu);
            shard.entries.clear();
            shard.lru.clear();
            shard.bytes = 0;
            for (auto& [k, flight] : shard.flights) {
                retired.push_back(flight.promise);
            }
            shard.flights.clear();
            ++shard.generation;
        }
        flushes_.fetch_add(1, std::memory_order_relaxed);
        for (auto& p : retired) {
            p->set_value(std::nullopt);
        }
#ifdef CPPPOOLS_USE_REDIS
        // A clear must not expose pre-clear Redis entries.  The minimal S3
        // backend has no namespace rotation yet, so retire it and continue
        // with an empty cold-memory cache instead of scanning Redis.
        if (cfg_.redis_store) cfg_.redis_store->disable();
#endif
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
            f.generation = shard.generation;
            return f;
        }
        const auto fit = shard.flights.find(k);
        if (fit != shard.flights.end()) {
            ++shard.stats.singleflight_merged;
            Flight f;
            f.future = fit->second.future;
            f.leader = false;
            f.flight_id = fit->second.flight_id;
            f.generation = fit->second.generation;
            return f;
        }
        auto promise = std::make_shared<std::promise<std::optional<RowData>>>();
        auto shared = std::make_shared<std::shared_future<std::optional<RowData>>>(
            promise->get_future().share());
        const std::uint64_t flight_id = ++shard.next_flight_id;
        shard.flights.emplace(k, Shard::FlightEntry{
            promise, shared, shard.generation, flight_id});
        ++shard.stats.misses;
        Flight f;
        f.future = shared;
        f.leader = true;
        f.flight_id = flight_id;
        f.generation = shard.generation;
        return f;
    }

    // leader 完成回源：同一临界区内原子完成「摘 flight + 世代比对 + 插入条目」，
    // 锁外兑现 future。世代比对与写入必须原子：若分两步（锁外检查 +
    // 锁内写），invalidate 可在两步之间插入，旧值照样写进缓存 ——
    // 正是 spec §4.3.1 要防的"旧值遮蔽已成功的写"。
    // 世代不匹配 → 丢弃回填并计 backfill_dropped；返回是否实际入缓存。
    bool finish_flight(const RowCacheKey& key, std::optional<RowData> result,
                       std::uint64_t expected_generation,
                       std::chrono::steady_clock::time_point cached_at = std::chrono::steady_clock::now()) {
        return finish_flight(key, std::move(result), expected_generation, 0, cached_at);
    }

    // 身份化完成：生产 leader 必须携带 join_flight 返回的 flight_id，
    // 防止迟到旧 leader 摘除已经替换的新 flight。
    bool finish_flight(const RowCacheKey& key, std::optional<RowData> result,
                       std::uint64_t expected_generation,
                       std::uint64_t flight_id,
                       std::chrono::steady_clock::time_point cached_at = std::chrono::steady_clock::now()) {
        const std::string k = key.encode();
        Shard& shard = shard_of(k);
        std::shared_ptr<std::promise<std::optional<RowData>>> promise;
        bool stored = false;
        bool stale = false;
#ifdef CPPPOOLS_USE_REDIS
        bool remote_store = false;
#endif
        {
            std::lock_guard<std::mutex> lock(shard.mu);
            const auto fit = shard.flights.find(k);
            if (flight_id != 0) {
                if (fit == shard.flights.end() || fit->second.flight_id != flight_id) {
                    stale = true;
                } else {
                    promise = fit->second.promise;
                    stale = fit->second.generation != expected_generation;
                    shard.flights.erase(fit);
                }
            } else if (fit != shard.flights.end()) {
                promise = fit->second.promise;
                stale = fit->second.generation != expected_generation;
                shard.flights.erase(fit);
            }
            if (result.has_value()) {
                if (!stale && shard.generation == expected_generation) {
#ifdef CPPPOOLS_USE_REDIS
                    remote_store = cfg_.redis_store && cfg_.redis_store->available() &&
                                   row_bytes(*result) <= cfg_.max_entry_bytes;
                    stored = remote_store || put_locked(shard, k, *result, cached_at);
#else
                    stored = put_locked(shard, k, *result, cached_at);
#endif
                } else {
                    stale = true;
                    ++shard.stats.backfill_dropped;
                }
            }
        }
#ifdef CPPPOOLS_USE_REDIS
        if (remote_store && result.has_value()) {
            if (!cfg_.redis_store->put(redis_key(k), encode_row(*result), cfg_.ttl)) {
                Shard& fallback = shard_of(k);
                std::lock_guard<std::mutex> lock(fallback.mu);
                if (fallback.generation == expected_generation) {
                    put_locked(fallback, k, *result, cached_at);
                }
            }
        }
#endif
        if (promise) {
            promise->set_value(stale ? std::nullopt : std::move(result));
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
        total.flushes = flushes_.load(std::memory_order_relaxed);
        return total;
    }

    // 运行时开关（kill-switch，spec §8.3）：关闭后装饰器整体直通内层。
    // const：指标/控制路径经 const 引用访问；原子写本身是线程安全的。
    void set_enabled(bool enabled) const { enabled_.store(enabled, std::memory_order_relaxed); }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

private:
    static void append_u32(std::string& out, std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<char>((value >> shift) & 0xffu));
        }
    }

    static bool read_u32(const std::string& in, std::size_t* offset, std::uint32_t* value) {
        if (*offset > in.size() || in.size() - *offset < 4) return false;
        *value = static_cast<std::uint32_t>(static_cast<unsigned char>(in[*offset])) |
                 (static_cast<std::uint32_t>(static_cast<unsigned char>(in[*offset + 1])) << 8) |
                 (static_cast<std::uint32_t>(static_cast<unsigned char>(in[*offset + 2])) << 16) |
                 (static_cast<std::uint32_t>(static_cast<unsigned char>(in[*offset + 3])) << 24);
        *offset += 4;
        return true;
    }

    static void append_string(std::string& out, const std::string& value) {
        append_u32(out, static_cast<std::uint32_t>(value.size()));
        out.append(value);
    }

    static bool read_string(const std::string& in, std::size_t* offset, std::string* value) {
        std::uint32_t length = 0;
        if (!read_u32(in, offset, &length) || length > in.size() - *offset) return false;
        value->assign(in.data() + *offset, length);
        *offset += length;
        return true;
    }

    static std::string encode_row(const RowData& row) {
        std::string out = "CPR1";
        append_u32(out, static_cast<std::uint32_t>(row.size()));
        for (const auto& [column, value] : row) {
            append_string(out, column);
            if (is_null(value)) {
                out.push_back('N');
            } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
                out.push_back('I');
                out.append(reinterpret_cast<const char*>(integer), sizeof(*integer));
            } else if (const auto* real = std::get_if<double>(&value)) {
                out.push_back('D');
                out.append(reinterpret_cast<const char*>(real), sizeof(*real));
            } else if (const auto* text = std::get_if<std::string>(&value)) {
                out.push_back('S');
                append_string(out, *text);
            }
        }
        return out;
    }

    static bool decode_row(const std::string& in, RowData* row) {
        if (in.size() < 8 || in.compare(0, 4, "CPR1") != 0) return false;
        std::size_t offset = 4;
        std::uint32_t count = 0;
        if (!read_u32(in, &offset, &count) || count > 100000) return false;
        row->clear();
        row->reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::string column;
            if (!read_string(in, &offset, &column) || offset >= in.size()) return false;
            const char type = in[offset++];
            if (type == 'N') {
                row->emplace_back(std::move(column), Value{});
            } else if (type == 'I' || type == 'D') {
                if (in.size() - offset < sizeof(std::int64_t)) return false;
                if (type == 'I') {
                    std::int64_t value = 0;
                    std::memcpy(&value, in.data() + offset, sizeof(value));
                    row->emplace_back(std::move(column), value);
                } else {
                    double value = 0;
                    std::memcpy(&value, in.data() + offset, sizeof(value));
                    row->emplace_back(std::move(column), value);
                }
                offset += sizeof(std::int64_t);
            } else if (type == 'S') {
                std::string value;
                if (!read_string(in, &offset, &value)) return false;
                row->emplace_back(std::move(column), std::move(value));
            } else {
                return false;
            }
        }
        return offset == in.size();
    }

    static std::string redis_key(const std::string& encoded) {
        return "cpppools:row:v1:" + encoded;
    }

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
        // mutable：缓存实例经 registry 以 const 指针暴露（kill-switch/
        // rejoin 需经 const 句柄改状态，与 enabled_ 同模式）。
        mutable std::unordered_map<std::string, Entry> entries;
        mutable std::list<std::string> lru;  // 头部 = 最近使用
        mutable std::size_t bytes{0};
        mutable std::uint64_t generation{0};
        // 键 → (promise, shared_future)：leader 兑现，等待者共享。
        struct FlightEntry {
            std::shared_ptr<std::promise<std::optional<RowData>>> promise;
            std::shared_ptr<std::shared_future<std::optional<RowData>>> future;
            std::uint64_t generation{0};
            std::uint64_t flight_id{0};
        };
        std::uint64_t next_flight_id{0};
        mutable std::unordered_map<std::string, FlightEntry> flights;
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
    bool put_locked(Shard& shard, const std::string& k, RowData row,
                    std::chrono::steady_clock::time_point cached_at) {
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
        shard.entries.emplace(k, Entry{std::move(row), cached_at,
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
    // 缓存级 flush 计数（clear 一次计一次，与分片无关；mutable 同 enabled_）。
    mutable std::atomic<std::uint64_t> flushes_{0};
};

}  // namespace cpppools::db
