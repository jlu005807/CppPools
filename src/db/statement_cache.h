#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

namespace cpppools::db {

// 每连接的 prepared statement 缓存（LRU，固定容量）。
//
// 为什么放在连接上：prepared statement 是**每连接资源**，
// 连接被池复用时 statement 也随之复用 —— 连接池的收益从
// “省一次握手”升级为“省一次握手 + 省一次 SQL 编译”。
//
// 键是 SQL 文本。本中间件的约定：do_select/do_insert 始终按 schema
// 全列生成固定 SQL（每表每操作恰好一条，列投影在取回后由适配层完成），
// 因此键空间有界（表数 × 操作数，个位数）；若将来引入按列子集生成
// SQL 的路径，键空间会变组合级，需重新审视淘汰策略。
// 容量满时整体清空（精确 LRU 淘汰的复杂度不值得：键空间有界时
// 正常运行不会触发淘汰；触发了也只是重编译几条语句）。
//
// 非线程安全：单个缓存实例只被一条连接（一个使用者）使用。
template <typename Stmt>
class StatementCache {
public:
    explicit StatementCache(std::size_t capacity = 64) : capacity_(capacity) {
        // 0 容量会使缓存恒不命中（每次 miss 都触发清空），属误用。
        assert(capacity > 0);
    }

    // 取已缓存的 statement；没有则用 make() 创建并缓存。
    // make() 失败的处理由 Stmt 自身表达（如空句柄），缓存照常存放。
    //
    // 契约：返回的引用在下一次 get_or_create 调用后即可能失效
    // （触发淘汰时 entry 被销毁）——禁止跨调用持有，使用模式必须是
    // “取一条 → bind → step → 结束”的单语句节奏。
    template <typename Make>
    Stmt& get_or_create(const std::string& key, Make&& make) {
        const auto it = cache_.find(key);
        if (it != cache_.end()) {
            // 命中：移到 LRU 头部。
            ++hits_;
            order_.splice(order_.begin(), order_, it->second.iter);
            return it->second.stmt;
        }
        ++misses_;
        if (order_.size() >= capacity_) {
            evict_all();
        }
        order_.push_front(key);
        const auto inserted = cache_.emplace(
            key, Entry{std::forward<Make>(make)(), order_.begin()});
        return inserted.first->second.stmt;
    }

    std::size_t size() const { return cache_.size(); }

    std::uint64_t hits() const { return hits_; }
    std::uint64_t misses() const { return misses_; }

private:
    struct Entry {
        Stmt stmt;
        typename std::list<std::string>::iterator iter;
    };

    void evict_all() {
        cache_.clear();
        order_.clear();
    }

    std::size_t capacity_;
    std::unordered_map<std::string, Entry> cache_;
    std::list<std::string> order_;  // 头部最近使用
    std::uint64_t hits_{0};
    std::uint64_t misses_{0};
};

}  // namespace cpppools::db
