#pragma once

#include "cpppools/cache/redis_transport.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

namespace cpppools::cache {

enum class RedisStoreCode { hit, miss, unavailable };

struct RedisStoreResult {
    RedisStoreCode code{RedisStoreCode::unavailable};
    std::string value;
};

// One datasource-scoped, one-way Redis cache backend.  The transport is
// serialized because it owns one RESP connection; a failed operation retires
// this instance permanently and lets RowCache continue in cold memory mode.
class RedisRowStore {
public:
    RedisRowStore(std::string host, int port, RedisLimits limits);

    bool available() const { return available_.load(std::memory_order_acquire); }
    void disable() { retire(); }
    RedisStoreResult get(const std::string& key);
    bool put(const std::string& key, const std::string& value,
             std::chrono::milliseconds ttl);
    bool erase(const std::string& key);

private:
    void retire();

    mutable std::mutex mu_;
    RedisTransport transport_;
    std::atomic<bool> available_{false};
};

}  // namespace cpppools::cache
