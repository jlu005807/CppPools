#include "cpppools/cache/redis_transport.h"

#include <cassert>
#include <chrono>
#include <string>

int main() {
    cpppools::cache::RedisLimits limits;
    limits.max_reply_bytes = 16;
    limits.operation_timeout = std::chrono::milliseconds(50);
    cpppools::cache::RedisTransport transport("127.0.0.1", 1, limits);
    assert(!transport.valid());
    const auto result = transport.ping();
    assert(result.code == cpppools::cache::RedisCode::unavailable ||
           result.code == cpppools::cache::RedisCode::timeout);
    assert(result.message.find("Redis") != std::string::npos ||
           result.message.find("redis") != std::string::npos);
    return 0;
}
