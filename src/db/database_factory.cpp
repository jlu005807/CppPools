#include "cpppools/db/database_factory.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace cpppools::db {

namespace {

// 注册表：函数局部 static（C++11 起线程安全初始化）。
// 注册/注销发生在启动期（单线程），运行期只读 —— mutex 只是防御性保护。
std::unordered_map<std::string, DatabaseFactoryFn>& registry() {
    static std::unordered_map<std::string, DatabaseFactoryFn> map;
    return map;
}

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

}  // namespace

bool register_database_type(const std::string& type, DatabaseFactoryFn fn) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    return registry().emplace(type, fn).second;  // 已存在则失败
}

bool unregister_database_type(const std::string& type) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    return registry().erase(type) > 0;
}

DatabaseCreateResult create_database(const config::DataSourceConfig& cfg) {
    DatabaseFactoryFn fn = nullptr;
    {
        std::lock_guard<std::mutex> lock(registry_mutex());
        const auto it = registry().find(cfg.type);
        if (it == registry().end()) {
            return {nullptr, "unknown database type \"" + cfg.type + "\""};
        }
        fn = it->second;
    }
    DatabaseCreateResult result = fn(cfg);
    // 防御：工厂约定失败时 db 为空且 error 非空，防止两个都空的静默失败。
    if (result.db == nullptr && result.error.empty()) {
        result.error = "database type \"" + cfg.type + "\" factory failed without an error message";
    }
    return result;
}

}  // namespace cpppools::db
