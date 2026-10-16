#include "cpppools/cache/redis_transport.h"

#include <cassert>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
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
        const int count = recv(socket, data + offset,
                               static_cast<int>(size - offset), 0);
#else
        const ssize_t count = recv(socket, data + offset, size - offset, 0);
#endif
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool read_line(TestSocket socket, std::string* line) {
    line->clear();
    char byte = 0;
    while (line->size() < 4096) {
        if (!read_exact(socket, &byte, 1)) return false;
        if (byte == '\r') {
            if (!read_exact(socket, &byte, 1) || byte != '\n') return false;
            return true;
        }
        line->push_back(byte);
    }
    return false;
}

bool write_all(TestSocket socket, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
#ifdef _WIN32
        const int count = send(socket, data.data() + offset,
                               static_cast<int>(data.size() - offset), 0);
#else
        const ssize_t count = send(socket, data.data() + offset,
                                   data.size() - offset, MSG_NOSIGNAL);
#endif
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

class RedisFixture {
public:
    explicit RedisFixture(bool delay_ping = false) : delay_ping_(delay_ping) {
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
            std::string line;
            if (!read_line(client, &line) || line.empty() || line[0] != '*') break;
            const int count = std::stoi(line.substr(1));
            std::vector<std::string> args;
            for (int i = 0; i < count; ++i) {
                if (!read_line(client, &line) || line.empty() || line[0] != '$') return;
                const std::size_t length = static_cast<std::size_t>(std::stoul(line.substr(1)));
                std::string value(length, '\0');
                if (!read_exact(client, value.data(), value.size())) return;
                char crlf[2]{};
                if (!read_exact(client, crlf, 2) || crlf[0] != '\r' || crlf[1] != '\n') return;
                args.push_back(std::move(value));
            }
            if (args.empty()) break;
            if (args[0] == "PING") {
                if (delay_ping_) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!write_all(client, "+PONG\r\n")) break;
            } else if (args[0] == "SET" && args.size() == 5) {
                values_[args[1]] = args[2];
                if (!write_all(client, "+OK\r\n")) break;
            } else if (args[0] == "GET" && args.size() == 2) {
                const auto it = values_.find(args[1]);
                if (it == values_.end()) {
                    if (!write_all(client, "$-1\r\n")) break;
                } else {
                    const std::string response = "$" + std::to_string(it->second.size()) +
                                                 "\r\n" + it->second + "\r\n";
                    if (!write_all(client, response)) break;
                }
            } else if (args[0] == "DEL" && args.size() == 2) {
                const auto removed = values_.erase(args[1]);
                if (!write_all(client, ":" + std::to_string(removed) + "\r\n")) break;
            } else if (!write_all(client, "-ERR unsupported\r\n")) {
                break;
            }
        }
        close_socket(client);
    }

    TestSocket listener_{kInvalidSocket};
    int port_{0};
    bool delay_ping_{false};
    std::map<std::string, std::string> values_;
    std::thread worker_;
};

}  // namespace

int main() {
    cpppools::cache::RedisLimits limits;
    limits.max_reply_bytes = 4096;
    limits.operation_timeout = std::chrono::milliseconds(100);
    RedisFixture fixture;
    cpppools::cache::RedisTransport transport("127.0.0.1", fixture.port(), limits);
    assert(transport.valid());
    assert(transport.ping().ok());
    assert(transport.get("missing").code == cpppools::cache::RedisCode::not_found);
    assert(transport.set_px("key", "value", std::chrono::milliseconds(1000)).ok());
    assert(transport.set_px("key", "value", std::chrono::milliseconds(0)).code ==
           cpppools::cache::RedisCode::protocol_error);
    const auto hit = transport.get("key");
    assert(hit.ok() && hit.value == "value");
    assert(transport.del("key").ok());
    assert(transport.get("key").code == cpppools::cache::RedisCode::not_found);

    RedisFixture delayed(true);
    cpppools::cache::RedisLimits short_limits;
    short_limits.operation_timeout = std::chrono::milliseconds(20);
    cpppools::cache::RedisTransport timeout_transport("127.0.0.1", delayed.port(), short_limits);
    assert(timeout_transport.valid());
    assert(timeout_transport.ping().code == cpppools::cache::RedisCode::timeout);

    RedisFixture capped_fixture;
    cpppools::cache::RedisLimits capped_limits;
    capped_limits.operation_timeout = std::chrono::milliseconds(100);
    capped_limits.max_reply_bytes = 4;
    cpppools::cache::RedisTransport capped_transport(
        "127.0.0.1", capped_fixture.port(), capped_limits);
    assert(capped_transport.valid());
    assert(capped_transport.set_px("large", "12345", std::chrono::milliseconds(1000)).ok());
    assert(capped_transport.get("large").code == cpppools::cache::RedisCode::limit_exceeded);
    return 0;
}
