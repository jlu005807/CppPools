#include "cache/redis_row_store.h"
#include "db/row_cache.h"

#include <cassert>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
using TestSocket = SOCKET;
constexpr TestSocket kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using TestSocket = int;
constexpr TestSocket kInvalidSocket = -1;
#endif

namespace {
void close_socket(TestSocket socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

bool read_exact(TestSocket socket, char* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
#ifdef _WIN32
        const int count = recv(socket, data + offset, static_cast<int>(size - offset), 0);
#else
        const ssize_t count = recv(socket, data + offset, size - offset, 0);
#endif
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool line(TestSocket socket, std::string* result) {
    result->clear();
    char byte = 0;
    while (result->size() < 4096) {
        if (!read_exact(socket, &byte, 1)) return false;
        if (byte == '\r') {
            if (!read_exact(socket, &byte, 1) || byte != '\n') return false;
            return true;
        }
        result->push_back(byte);
    }
    return false;
}

bool write_all(TestSocket socket, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
#ifdef _WIN32
        const int count = send(socket, data.data() + offset, static_cast<int>(data.size() - offset), 0);
#else
        const ssize_t count = send(socket, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
#endif
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

class RedisFixture {
public:
    explicit RedisFixture(bool close_after_ping = false)
        : close_after_ping_(close_after_ping) {
#ifdef _WIN32
        WSADATA data{};
        assert(WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(listener_ != kInvalidSocket);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(0);
        assert(bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        assert(listen(listener_, 1) == 0);
        int length = sizeof(address);
        assert(getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { serve(); });
    }
    ~RedisFixture() {
        if (listener_ != kInvalidSocket) close_socket(listener_);
        if (worker_.joinable()) worker_.join();
#ifdef _WIN32
        WSACleanup();
#endif
    }
    int port() const { return port_; }

private:
    void serve() {
        const TestSocket client = accept(listener_, nullptr, nullptr);
        if (client == kInvalidSocket) return;
        while (true) {
            std::string header;
            if (!line(client, &header) || header.empty() || header[0] != '*') break;
            const int count = std::stoi(header.substr(1));
            std::vector<std::string> args;
            for (int i = 0; i < count; ++i) {
                if (!line(client, &header) || header.empty() || header[0] != '$') return;
                const std::size_t length = static_cast<std::size_t>(std::stoul(header.substr(1)));
                std::string value(length, '\0');
                if (!read_exact(client, value.data(), value.size())) return;
                char crlf[2]{};
                if (!read_exact(client, crlf, 2)) return;
                args.push_back(std::move(value));
            }
            if (args.empty()) break;
            if (args[0] == "PING") {
                if (!write_all(client, "+PONG\r\n")) break;
                if (close_after_ping_) break;
            } else if (args[0] == "SET") {
                values_[args[1]] = args[2];
                if (!write_all(client, "+OK\r\n")) break;
            } else if (args[0] == "GET") {
                const auto it = values_.find(args[1]);
                if (it == values_.end()) {
                    if (!write_all(client, "$-1\r\n")) break;
                } else if (!write_all(client, "$" + std::to_string(it->second.size()) +
                                              "\r\n" + it->second + "\r\n")) break;
            } else if (args[0] == "DEL") {
                const auto removed = values_.erase(args[1]);
                if (!write_all(client, ":" + std::to_string(removed) + "\r\n")) break;
            }
        }
        close_socket(client);
    }
    TestSocket listener_{kInvalidSocket};
    int port_{0};
    std::map<std::string, std::string> values_;
    bool close_after_ping_{false};
    std::thread worker_;
};
}  // namespace

int main() {
    RedisFixture fixture;
    cpppools::cache::RedisLimits limits;
    limits.operation_timeout = std::chrono::milliseconds(100);
    auto store = std::make_shared<cpppools::cache::RedisRowStore>(
        "127.0.0.1", fixture.port(), limits);
    assert(store->available());

    cpppools::db::RowCacheConfig config;
    config.shard_count = 1;
    config.ttl = std::chrono::seconds(1);
    config.redis_store = store;
    cpppools::db::RowCache cache(config);
    const cpppools::db::RowCacheKey key{"main", "users", "id", std::int64_t{7}};
    const cpppools::db::RowData row{{"id", std::int64_t{7}}, {"name", std::string("alice")}};

    assert(!cache.get(key).has_value());
    cache.put(key, row);
    const auto hit = cache.get(key);
    assert(hit.has_value() && *hit == row);
    cache.invalidate(key);
    assert(!cache.get(key).has_value());
    cache.put(key, row);
    cache.clear();
    assert(!cache.get(key).has_value());

    store->disable();
    cache.put(key, row);
    const auto fallback = cache.get(key);
    assert(fallback.has_value() && *fallback == row);

    RedisFixture failing(true);
    auto failing_store = std::make_shared<cpppools::cache::RedisRowStore>(
        "127.0.0.1", failing.port(), limits);
    assert(failing_store->available());
    cpppools::db::RowCacheConfig cold_config;
    cold_config.shard_count = 1;
    cold_config.redis_store = failing_store;
    cpppools::db::RowCache cold_cache(cold_config);
    cold_cache.put(key, row);
    assert(cold_cache.get(key).has_value());
    return 0;
}
