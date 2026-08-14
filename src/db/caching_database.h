#pragma once

#include "db/row_cache.h"

#include "cpppools/db/database.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cpppools::db {

// 缓存装饰器：包在真实后端外，select 走行级缓存，写走穿透 + 失效。
//
// 位置（spec §4.1）：校验管道（基类 public）→ 本装饰器 do_* → 内层后端。
// 缓存是执行策略、校验是准入策略，正交 —— 装饰器拿到的请求已过管道。
//
// 共享粒度（spec §4.1.1）：RowCache 由 registry 按数据源创建并注入该源
// 全部槽位的装饰器 —— 跨连接共享同一份缓存。per-connection 缓存会
// 导致"写失效只清 1/N 份副本"，单节点内强一致即被打破（评审 B1）。
//
// 一致性机制：
// - select miss → Singleflight 合并回源；回填在 RowCache 分片锁内做世代号
//   CAS（spec §4.3.1），失效后到达的回填直接丢弃，防旧值遮蔽已成功的写；
// - 写穿透双删（spec §4.3）：前置删 → 落库 → 后置删，失效在落库成功后
//   无条件再执行一次（不变量 I0）；世代 CAS 另防"写后到达的回填"；
// - REAL 键列 bypass 缓存（浮点键字符串化不稳定，spec §5.5）；
// - 失效永不 bypass：kill-switch 关闭时读直通内层，但写的失效仍执行。
class CachingDatabase : public Database {
public:
    CachingDatabase(std::unique_ptr<Database> inner, std::shared_ptr<RowCache> cache)
        : Database(inner->schema(), inner->name()),
          inner_(std::move(inner)),
          cache_(std::move(cache)) {}

    bool healthy() override { return inner_->healthy(); }
    std::string backend_type() const override { return inner_->backend_type(); }

    RowCache* cache() { return cache_.get(); }

protected:
    DbResult<ResultSet> do_select(const SelectRequest& req) override {
        // bypass 三态：缓存关闭 / REAL 键列 / where 列查不到（不应发生，防御）。
        if (cache_ == nullptr || !cache_->enabled()) {
            return inner_->select(req);  // 内层 public：校验幂等
        }
        const ColumnInfo* where_col = schema_.find_column(req.table, req.where.column);
        if (where_col == nullptr || where_col->kind == ValueKind::kReal) {
            return inner_->select(req);
        }
        const RowCacheKey key{name(), req.table, req.where.column, req.where.value};

        if (auto row = cache_->get(key)) {
            return make_hit(req, std::move(*row));
        }
        // miss：Singleflight 合并回源。
        RowCache::Flight flight = cache_->join_flight(key);
        if (!flight.leader) {
            if (auto row = flight.future->get()) {
                return make_hit(req, std::move(*row));
            }
            // leader 未缓存（0 行 / 错误 / 被失效丢弃）：自己回源。
            return inner_->select(req);
        }
        // leader：先记世代 → 回源 → 锁内原子 CAS 回填或丢弃。
        const std::uint64_t seq0 = cache_->generation(key);
        // RAII 守卫：回源/转换路径若抛异常，兜底兑现 promise 并摘 flight，
        // 否则等待者的 future->get() 永久阻塞（占住 worker 与池槽位）。
        struct LeaderGuard {
            RowCache* cache;
            RowCacheKey key;
            std::uint64_t gen;
            bool finished{false};
            ~LeaderGuard() {
                if (!finished) {
                    cache->finish_flight(key, std::nullopt, gen);
                }
            }
        } guard{cache_.get(), key, seq0};

        DbResult<ResultSet> result = inner_->select(req);
        if (result.status == DbStatus::kOk && !result.value.rows.empty()) {
            RowData row = to_row_data(result);
            // 世代比对在 finish_flight 的临界区内与写入原子完成；
            // 被拦下的回填由 RowCache 计 backfill_dropped（§8.1 导出）。
            cache_->finish_flight(key, std::move(row), seq0);
        } else {
            cache_->finish_flight(key, std::nullopt, seq0);
        }
        guard.finished = true;
        return result;
    }

    DbResult<InsertResult> do_insert(const InsertRequest& req) override {
        // 双删前置删（spec §4.3）：落库前先失效新行可达的唯一键，
        // 覆盖"读回填 → 写落库"窗口内的并发回填（rowid 复用复活旧条目场景）。
        invalidate_unique_keys(req.table, req.values);
        DbResult<InsertResult> result = inner_->insert(req);
        if (result.status == DbStatus::kOk) {
            // 后置删：失效新行可达的全部唯一键（含自增 id）。
            const TableInfo* table = schema_.find_table(req.table);
            if (table != nullptr) {
                for (const auto& col : table->columns) {
                    if (!col.unique) {
                        continue;
                    }
                    const Value* given = find_value(req.values, col.name);
                    if (given != nullptr) {
                        cache_->invalidate(RowCacheKey{name(), req.table, col.name, *given});
                    } else if (col.autoincrement && result.value.inserted_id.has_value()) {
                        cache_->invalidate(RowCacheKey{
                            name(), req.table, col.name, Value{*result.value.inserted_id}});
                    }
                }
            }
        }
        return result;
    }

    DbResult<RowCount> do_update(const UpdateRequest& req) override {
        // 双删前置删（spec §4.3）。
        cache_->invalidate(RowCacheKey{name(), req.table, req.where.column,
                                       req.where.value});
        DbResult<RowCount> result = inner_->update(req);
        if (result.status == DbStatus::kOk) {
            // 后置删（无条件执行，I0）：0 行更新未改 DB，本可不失效，
            // 此处口径取宽 —— 方向安全，代价仅失效计数放大。
            cache_->invalidate(RowCacheKey{name(), req.table, req.where.column,
                                           req.where.value});
            // 被改写的唯一键：新值条目也要失效（旧值条目的残留由 TTL 封顶，
            // 装饰器无法得知旧值，spec §4.3.2 已声明此残留）。
            const TableInfo* table = schema_.find_table(req.table);
            if (table != nullptr) {
                for (const auto& [col, value] : req.set) {
                    const ColumnInfo* info = schema_.find_column(req.table, col);
                    if (info != nullptr && info->unique) {
                        cache_->invalidate(RowCacheKey{name(), req.table, col, value});
                    }
                }
            }
        }
        return result;
    }

    DbResult<RowCount> do_remove(const DeleteRequest& req) override {
        // 双删前置删（spec §4.3）。
        cache_->invalidate(RowCacheKey{name(), req.table, req.where.column,
                                       req.where.value});
        DbResult<RowCount> result = inner_->remove(req);
        if (result.status == DbStatus::kOk) {
            cache_->invalidate(RowCacheKey{name(), req.table, req.where.column,
                                           req.where.value});
        }
        return result;
    }

private:
    // insert 前置删辅助：失效给定值覆盖的全部唯一键（自增 id 落库前未知，跳过）。
    void invalidate_unique_keys(const std::string& table,
                                const std::vector<std::pair<std::string, Value>>& values) {
        const TableInfo* t = schema_.find_table(table);
        if (t == nullptr) {
            return;
        }
        for (const auto& col : t->columns) {
            if (!col.unique) {
                continue;
            }
            const Value* given = find_value(values, col.name);
            if (given != nullptr) {
                cache_->invalidate(RowCacheKey{name(), table, col.name, *given});
            }
        }
    }

    // 命中路径：整行条目 → 按请求列投影（复现三条可观测语义：
    // 投影顺序 = 请求顺序；重复列不去重；全列 = schema 顺序且空行也带列名）。
    DbResult<ResultSet> make_hit(const SelectRequest& req, RowData&& row) const {
        auto find_cell = [&row](const std::string& col) -> const Value* {
            for (const auto& [name, v] : row) {
                if (name == col) {
                    return &v;
                }
            }
            return nullptr;
        };
        ResultSet rs;
        if (req.columns.empty()) {
            // 全列：schema 顺序（校验管道已保证表存在）。
            const TableInfo* t = schema_.find_table(req.table);
            if (t == nullptr) {
                return {DbStatus::kBackendError, ResultSet{}, "cache hit for unknown table"};
            }
            for (const auto& c : t->columns) {
                rs.columns.push_back(c.name);
            }
        } else {
            rs.columns = req.columns;  // 请求顺序 + 重复列不去重
        }
        std::vector<Value> single;
        single.reserve(rs.columns.size());
        for (const auto& col : rs.columns) {
            const Value* cell = find_cell(col);
            single.push_back(cell != nullptr ? *cell : Value{});
        }
        rs.rows.push_back(std::move(single));
        return {DbStatus::kOk, std::move(rs), ""};
    }

    static RowData to_row_data(const DbResult<ResultSet>& result) {
        RowData row;
        const ResultSet& rs = result.value;
        if (!rs.rows.empty()) {
            const auto& first = rs.rows[0];
            row.reserve(rs.columns.size());
            for (std::size_t i = 0; i < rs.columns.size() && i < first.size(); ++i) {
                row.emplace_back(rs.columns[i], first[i]);
            }
        }
        return row;
    }

    static const Value* find_value(
        const std::vector<std::pair<std::string, Value>>& pairs, const std::string& col) {
        for (const auto& [name, v] : pairs) {
            if (name == col) {
                return &v;
            }
        }
        return nullptr;
    }

    std::unique_ptr<Database> inner_;
    std::shared_ptr<RowCache> cache_;
};

}  // namespace cpppools::db
