#include "cache/redis_row_store.h"

#include <utility>

namespace cpppools::cache {

RedisRowStore::RedisRowStore(std::string host, int port, RedisLimits limits)
    : transport_(std::move(host), port, limits) {
    if (transport_.valid() && transport_.ping().ok()) {
        available_.store(true, std::memory_order_release);
    }
}

void RedisRowStore::retire() {
    available_.store(false, std::memory_order_release);
}

RedisStoreResult RedisRowStore::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!available_.load(std::memory_order_acquire)) return {};
    const RedisResult result = transport_.get(key);
    if (result.code == RedisCode::not_found) {
        return {RedisStoreCode::miss, {}};
    }
    if (!result.ok()) {
        retire();
        return {};
    }
    return {RedisStoreCode::hit, result.value};
}

bool RedisRowStore::put(const std::string& key, const std::string& value,
                        std::chrono::milliseconds ttl) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!available_.load(std::memory_order_acquire)) return false;
    if (!transport_.set_px(key, value, ttl).ok()) {
        retire();
        return false;
    }
    return true;
}

bool RedisRowStore::erase(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!available_.load(std::memory_order_acquire)) return false;
    if (!transport_.del(key).ok()) {
        retire();
        return false;
    }
    return true;
}

}  // namespace cpppools::cache
