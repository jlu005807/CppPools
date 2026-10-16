#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cpppools::cache {

enum class RedisCode { ok, not_found, unavailable, timeout, protocol_error, limit_exceeded };

struct RedisResult {
    RedisCode code{RedisCode::unavailable};
    std::string message;
    std::string value;

    bool ok() const { return code == RedisCode::ok; }
};

struct RedisLimits {
    std::chrono::milliseconds operation_timeout{100};
    std::size_t max_reply_bytes{1024 * 1024};
};

class RedisTransport {
public:
    RedisTransport(std::string host, int port, RedisLimits limits = {});
    ~RedisTransport();

    RedisTransport(const RedisTransport&) = delete;
    RedisTransport& operator=(const RedisTransport&) = delete;

    bool valid() const { return fd_ != -1; }
    RedisResult ping();
    RedisResult get(const std::string& key);
    RedisResult set_px(const std::string& key, const std::string& value,
                       std::chrono::milliseconds ttl);
    RedisResult del(const std::string& key);

private:
    RedisResult command(const std::string& command);
    RedisResult command(const std::vector<std::string>& args);
    RedisResult read_reply(std::chrono::steady_clock::time_point deadline);
    bool wait_io(bool write, std::chrono::steady_clock::time_point deadline);
    bool write_all(const std::string& bytes,
                   std::chrono::steady_clock::time_point deadline);
    void close();

    std::string host_;
    int port_;
    RedisLimits limits_;
    std::intptr_t fd_{-1};
};

}  // namespace cpppools::cache
