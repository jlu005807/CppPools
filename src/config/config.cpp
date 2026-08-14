#include "cpppools/config/config.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>

namespace cpppools::config {

namespace {

using njson = nlohmann::json;

// 配置文件大小上限：运维提供的本地文件不该超过这个量级。
// 防御误配置（如把日志文件当配置），与 fail-fast 哲学一致。
constexpr std::streamoff kMaxConfigBytes = static_cast<std::streamoff>(1) << 20;  // 1MB

// 带类型守卫的字段读取。
//
// 为什么不用 nlohmann 的 get<T>() 裸调：类型不符时它抛 type_error，
// 消息里没有字段名（"[json.exception.type_error.302] type must be number, but is string"），
// 且会穿透 load_config 直达 main —— 配置错误必须以可读错误 fail-fast，而不是 abort。
// 这里类型不符时抛带字段名的 std::runtime_error，由 load_config 统一捕获转 fail。
// ctx 是字段路径前缀（如 "server." / "datasources[0].mysql."），用于错误消息。
template <typename T>
T get_num(const njson& obj, const char* key, T fallback, const std::string& ctx) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return fallback;
    }
    if constexpr (std::is_same<T, bool>::value) {
        if (!it->is_boolean()) {
            throw std::runtime_error("\"" + ctx + key + "\" must be a boolean");
        }
    } else {
        // is_number_integer 同时拦掉字符串与浮点（"port": 8080.5 不该静默截断）。
        if (!it->is_number_integer()) {
            throw std::runtime_error("\"" + ctx + key + "\" must be an integer");
        }
    }
    return it->get<T>();
}

std::string get_str(const njson& obj, const char* key, const std::string& fallback,
                    const std::string& ctx) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return fallback;
    }
    if (!it->is_string()) {
        throw std::runtime_error("\"" + ctx + key + "\" must be a string");
    }
    return it->get<std::string>();
}

ConfigLoadResult fail(std::string message) {
    ConfigLoadResult r;
    r.ok = false;
    r.error = std::move(message);
    return r;
}

}  // namespace

std::string resolve_config_path(int argc, const char* const* argv) {
    if (argc >= 3 && argv != nullptr && std::string(argv[1]) == "--config") {
        return argv[2];
    }
    if (const char* env = std::getenv("CPPPOOLS_CONFIG"); env != nullptr && env[0] != '\0') {
        return env;
    }
    return "config/cpppools.json";
}

ConfigLoadResult load_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return fail("config file not found: " + path +
                    " (use --config <path> or CPPPOOLS_CONFIG; see config/cpppools.example.json)");
    }
    in.seekg(0, std::ios::end);
    const std::streamoff file_size = in.tellg();
    if (file_size > kMaxConfigBytes) {
        return fail("config file too large (" + std::to_string(static_cast<long long>(file_size)) +
                    " bytes, limit " + std::to_string(static_cast<long long>(kMaxConfigBytes)) + ")");
    }
    in.seekg(0, std::ios::beg);
    std::ostringstream buf;
    buf << in.rdbuf();

    njson root;
    try {
        root = njson::parse(buf.str());
    } catch (const njson::exception& ex) {
        return fail(std::string("malformed JSON in ") + path + ": " + ex.what());
    }
    if (!root.is_object()) {
        return fail("config root must be a JSON object");
    }

    // 字段级解析统一包在 try 里：get_num/get_str 的类型守卫抛带字段名的
    // runtime_error，这里转成 fail —— 任何字段类型/取值错误都是可读的 fail-fast，
    // 绝不允许异常穿出 load_config（那会变成 abort）。
    try {
        AppConfig cfg;

        // ---- server 段（可选，全字段有默认值）----
        if (const auto it = root.find("server"); it != root.end()) {
            if (!it->is_object()) {
                return fail("\"server\" must be an object");
            }
            const std::string ctx = "server.";
            const int port = get_num<int>(*it, "port", 8080, ctx);
            if (port <= 0 || port > 65535) {
                return fail("\"server.port\" must be in [1, 65535]");
            }
            cfg.server.port = port;
            const int workers = get_num<int>(*it, "worker_threads", 6, ctx);
            if (workers <= 0) {
                return fail("\"server.worker_threads\" must be > 0");
            }
            cfg.server.worker_threads = static_cast<std::size_t>(workers);
            const int window = get_num<int>(*it, "latency_window_size", 1024, ctx);
            if (window <= 0) {
                return fail("\"server.latency_window_size\" must be > 0");
            }
            cfg.server.latency_window_size = static_cast<std::size_t>(window);
        }

        // ---- datasources 段（必需，非空数组）----
        const auto ds_it = root.find("datasources");
        if (ds_it == root.end() || !ds_it->is_array()) {
            return fail("\"datasources\" is required and must be an array");
        }
        if (ds_it->empty()) {
            return fail("\"datasources\" must not be empty");
        }

        std::unordered_set<std::string> seen_names;
        std::size_t default_count = 0;

        for (std::size_t i = 0; i < ds_it->size(); ++i) {
            const njson& item = (*ds_it)[i];
            const std::string idx = std::to_string(i);
            const std::string ctx = "datasources[" + idx + "].";
            if (!item.is_object()) {
                return fail("datasources[" + idx + "] must be an object");
            }
            DataSourceConfig ds;
            ds.name = get_str(item, "name", "", ctx);
            if (ds.name.empty()) {
                return fail("datasources[" + idx + "] missing \"name\"");
            }
            if (!seen_names.insert(ds.name).second) {
                return fail("duplicate datasource name: \"" + ds.name + "\"");
            }
            ds.type = get_str(item, "type", "", ctx);
            ds.is_default = get_num<bool>(item, "default", false, ctx);
            const int pool_size = get_num<int>(item, "pool_size", 8, ctx);
            if (pool_size <= 0) {
                return fail("datasource \"" + ds.name + "\": pool_size must be > 0");
            }
            ds.pool_size = static_cast<std::size_t>(pool_size);
            if (ds.is_default) {
                ++default_count;
            }

            if (ds.type == "sqlite") {
                const auto sq = item.find("sqlite");
                if (sq == item.end() || !sq->is_object()) {
                    return fail("datasource \"" + ds.name + "\": missing \"sqlite\" section");
                }
                ds.sqlite_path = get_str(*sq, "path", "", ctx + "sqlite.");
                if (ds.sqlite_path.empty()) {
                    return fail("datasource \"" + ds.name + "\": sqlite.path is required");
                }
            } else if (ds.type == "mysql") {
                const auto my = item.find("mysql");
                if (my == item.end() || !my->is_object()) {
                    return fail("datasource \"" + ds.name + "\": missing \"mysql\" section");
                }
                const std::string mctx = ctx + "mysql.";
                ds.mysql_host = get_str(*my, "host", "127.0.0.1", mctx);
                const int mport = get_num<int>(*my, "port", 3306, mctx);
                if (mport <= 0 || mport > 65535) {
                    return fail("datasource \"" + ds.name + "\": mysql.port must be in [1, 65535]");
                }
                ds.mysql_port = mport;
                ds.mysql_user = get_str(*my, "user", "", mctx);
                ds.mysql_database = get_str(*my, "database", "", mctx);
                if (ds.mysql_user.empty()) {
                    return fail("datasource \"" + ds.name + "\": mysql.user is required");
                }
                if (ds.mysql_database.empty()) {
                    return fail("datasource \"" + ds.name + "\": mysql.database is required");
                }
                // 密码：password_env 优先（安全实践：不落盘），其次明文 password。
                const std::string env_name = get_str(*my, "password_env", "", mctx);
                if (!env_name.empty()) {
                    const char* env_val = std::getenv(env_name.c_str());
                    if (env_val == nullptr) {
                        return fail("datasource \"" + ds.name + "\": environment variable \"" +
                                    env_name + "\" (mysql.password_env) is not set");
                    }
                    ds.mysql_password = env_val;
                } else {
                    ds.mysql_password = get_str(*my, "password", "", mctx);
                }
            } else {
                return fail("datasource \"" + ds.name + "\": unknown type \"" + ds.type +
                            "\" (supported: sqlite, mysql)");
            }

            cfg.datasources.push_back(std::move(ds));
        }

        if (default_count > 1) {
            return fail("at most one datasource may have \"default\": true");
        }

        ConfigLoadResult result;
        result.ok = true;
        result.config = std::move(cfg);
        return result;
    } catch (const std::exception& ex) {
        return fail(std::string("invalid config: ") + ex.what());
    }
}

}  // namespace cpppools::config
