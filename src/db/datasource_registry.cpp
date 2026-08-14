#include "cpppools/db/datasource_registry.h"

#include "cpppools/db/database_factory.h"
#include "cpppools/db/db_connection_pool.h"
#include "db/caching_database.h"
#include "db/row_cache.h"
#include "server/hash_ring.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace cpppools::db {

namespace {

std::string join_names(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += names[i];
    }
    return out;
}

}  // namespace

DataSourceRegistry::~DataSourceRegistry() = default;

RegistryBuildResult DataSourceRegistry::build(
    const std::vector<config::DataSourceConfig>& sources,
    const config::AppConfig::Cache& cache_cfg) {
    if (sources.empty()) {
        return {nullptr, "config has no datasources; at least one is required"};
    }

    // 重名检查：数据源名是路由键，重复即配置错误。
    for (std::size_t i = 0; i < sources.size(); ++i) {
        for (std::size_t j = i + 1; j < sources.size(); ++j) {
            if (sources[i].name == sources[j].name) {
                return {nullptr, "duplicate datasource name \"" + sources[i].name + "\""};
            }
        }
    }

    // 默认源：唯一源即默认；多源必须恰有一个 is_default。
    std::string default_name;
    int defaults = 0;
    for (const auto& s : sources) {
        if (s.is_default) {
            ++defaults;
            default_name = s.name;
        }
    }
    if (sources.size() == 1) {
        default_name = sources[0].name;
    } else if (defaults != 1) {
        return {nullptr, "config has " + std::to_string(sources.size()) +
                             " datasources; exactly one must be marked \"default\": true (found " +
                             std::to_string(defaults) + ")"};
    }

    std::unique_ptr<DataSourceRegistry> reg(new DataSourceRegistry());

    for (const auto& cfg : sources) {
        // 探连：可连 + 可自省，并取表清单建索引。
        // probe 实例随后销毁；池内每槽位会各自重新连接 + 自省。
        DatabaseCreateResult probe = create_database(cfg);
        if (probe.db == nullptr) {
            return {nullptr, "datasource \"" + cfg.name + "\": " + probe.error};
        }

        Entry entry;
        entry.config = cfg;
        entry.tables = probe.db->schema().table_names();

        // :memory: 陷阱（spec §10.1）：每 open 一个独立内存库，
        // 多槽池 = 多个互不相干的空库 —— 强制单槽并警告。
        std::size_t pool_size = cfg.pool_size;
        if (cfg.type == "sqlite" && cfg.sqlite_path == ":memory:" && pool_size != 1) {
            std::fprintf(stderr,
                         "[warn] :memory: sqlite datasource \"%s\": forcing pool_size 1 "
                         "(each connection would otherwise see its own empty database; "
                         "note a health-check rebuild creates ANOTHER empty database, "
                         "losing all in-memory data)\n",
                         cfg.name.c_str());
            pool_size = 1;
        }

        // 该源的共享缓存（enabled 时）：全部槽位的装饰器共用一份（spec §4.1.1）。
        std::shared_ptr<RowCache> cache;
        if (cache_cfg.enabled) {
            RowCacheConfig rc;
            rc.max_entries = cache_cfg.max_entries;
            rc.max_bytes = cache_cfg.max_bytes;
            rc.max_entry_bytes = cache_cfg.max_entry_bytes;
            rc.ttl = std::chrono::milliseconds(cache_cfg.ttl_ms);
            cache = std::make_shared<RowCache>(rc);
        }
        const config::DataSourceConfig pool_cfg = cfg;  // 值拷贝：factory 捕获
        ConnectionFactory factory = [pool_cfg, cache](std::size_t) -> std::unique_ptr<Database> {
            DatabaseCreateResult result = create_database(pool_cfg);
            if (result.db == nullptr) {
                return nullptr;  // 池 fail-fast
            }
            if (cache) {
                return std::make_unique<CachingDatabase>(std::move(result.db), cache);
            }
            return std::move(result.db);
        };
        DbPoolConfig pcfg;
        pcfg.pool_size = pool_size;
        pcfg.backend = cfg.type;
        // 池构造失败（factory 抛异常/返回 nullptr）转成带源名的错误串：
        // build 的对外契约是"失败经 error 返回"，不让池的 runtime_error
        // 丢失"哪个源"上下文（probe 成功后库变不可连的窗口虽小，期6 MySQL 下会放大）。
        try {
            entry.pool = std::make_unique<DbConnectionPool>(cfg.name, std::move(factory), pcfg);
        } catch (const std::exception& ex) {
            return {nullptr, "datasource \"" + cfg.name + "\": pool init failed: " + ex.what()};
        }
        entry.cache = std::move(cache);

        for (const auto& table : entry.tables) {
            reg->table_index_[table].push_back(cfg.name);
        }
        reg->by_name_[cfg.name] = reg->entries_.size();
        reg->entries_.push_back(std::move(entry));
    }

    reg->default_name_ = std::move(default_name);

    // 歧义表启动期告警（spec §4.1：歧义必须在启动时被发现，
    // 而不是在某个生产请求上静默赌一把）。运行期这些表仍需显式 datasource。
    // 排序后打印：unordered_map 遍历序不稳定，日志行序跨运行应可复现。
    std::vector<std::string> ambiguous_tables;
    for (const auto& [table, owners] : reg->table_index_) {
        if (owners.size() > 1) {
            ambiguous_tables.push_back(table);
        }
    }
    std::sort(ambiguous_tables.begin(), ambiguous_tables.end());
    for (const auto& table : ambiguous_tables) {
        std::fprintf(stderr,
                     "[warn] table \"%s\" exists in multiple datasources (%s); "
                     "requests must specify \"datasource\" explicitly\n",
                     table.c_str(), join_names(reg->table_index_[table]).c_str());
    }
    return {std::move(reg), ""};
}

RouteResult DataSourceRegistry::route(const std::string& table,
                                      const std::string& datasource_hint) const {
    // 显式 datasource 优先级最高（spec §4.1）。
    if (!datasource_hint.empty()) {
        const Entry* entry = find_entry(datasource_hint);
        if (entry == nullptr) {
            RouteResult r;
            r.status = RouteStatus::kUnknownDatasource;
            r.message = "unknown datasource \"" + datasource_hint + "\"";
            return r;
        }
        const auto it = std::find(entry->tables.begin(), entry->tables.end(), table);
        if (it == entry->tables.end()) {
            RouteResult r;
            r.status = RouteStatus::kUnknownTable;
            r.message = "datasource \"" + datasource_hint + "\" has no table \"" + table + "\"";
            return r;
        }
        RouteResult r;
        r.status = RouteStatus::kOk;
        r.pool = entry->pool.get();
        r.datasource = datasource_hint;
        return r;
    }

    const auto it = table_index_.find(table);
    if (it == table_index_.end() || it->second.empty()) {
        RouteResult r;
        r.status = RouteStatus::kUnknownTable;
        r.message = "unknown table \"" + table + "\" (not present in any datasource)";
        return r;
    }
    if (it->second.size() > 1) {
        // 不兜底猜：选中谁取决于配置书写顺序，客户端无从预料，
        // 且 affected_rows:1 会让客户端误以为写对了库（spec §4.1）。
        RouteResult r;
        r.status = RouteStatus::kAmbiguous;
        r.candidates = it->second;
        r.message = "table \"" + table + "\" exists in multiple datasources (" +
                    join_names(it->second) + "); specify \"datasource\" explicitly";
        return r;
    }
    const Entry* entry = find_entry(it->second[0]);
    RouteResult r;
    r.status = RouteStatus::kOk;
    r.pool = entry->pool.get();
    r.datasource = it->second[0];
    return r;
}

DbConnectionPool* DataSourceRegistry::find_pool(const std::string& datasource_name) const {
    const Entry* entry = find_entry(datasource_name);
    return (entry != nullptr) ? entry->pool.get() : nullptr;
}

DbConnectionPool* DataSourceRegistry::default_pool() const {
    return find_pool(default_name_);
}

std::vector<std::string> DataSourceRegistry::datasource_names() const {
    std::vector<std::string> names;
    names.reserve(entries_.size());
    for (const auto& entry : entries_) {
        names.push_back(entry.config.name);
    }
    return names;
}

std::vector<const DbConnectionPool*> DataSourceRegistry::pools() const {
    std::vector<const DbConnectionPool*> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) {
        out.push_back(entry.pool.get());
    }
    return out;
}

std::vector<std::string> DataSourceRegistry::tables_of(
    const std::string& datasource_name) const {
    const Entry* entry = find_entry(datasource_name);
    return (entry != nullptr) ? entry->tables : std::vector<std::string>{};
}

std::vector<DataSourceRegistry::DataSourceDigest> DataSourceRegistry::datasource_digests() const {
    // 摘要只含"连的是哪个目标"（type + 库标识），**不含凭据**：
    // 某节点的 sqlite.path 写错时，SDK/运维比对全节点摘要即可发现
    // （spec §6.3：无强制手段的关键结构约束，由此提供检测）。
    using server::hash_ring::lp;
    std::vector<DataSourceDigest> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) {
        const config::DataSourceConfig& c = entry.config;
        std::string input = lp(c.type);
        if (c.type == "sqlite") {
            input += lp(c.sqlite_path);
        } else {
            input += lp(c.mysql_host);
            input += lp(std::to_string(c.mysql_port));
            input += lp(c.mysql_database);
        }
        const uint32_t h = server::hash_ring::murmur3_32(input);
        char hex[9];
        std::snprintf(hex, sizeof(hex), "%08x", h);
        out.push_back(DataSourceDigest{c.name, c.type, std::string(hex)});
    }
    return out;
}

std::vector<std::pair<std::string, const RowCache*>> DataSourceRegistry::caches() const {
    std::vector<std::pair<std::string, const RowCache*>> out;
    for (const auto& entry : entries_) {
        if (entry.cache) {
            out.emplace_back(entry.config.name, entry.cache.get());
        }
    }
    return out;
}

const DataSourceRegistry::Entry* DataSourceRegistry::find_entry(
    const std::string& name) const {
    const auto it = by_name_.find(name);
    if (it == by_name_.end()) {
        return nullptr;
    }
    return &entries_[it->second];
}

}  // namespace cpppools::db
