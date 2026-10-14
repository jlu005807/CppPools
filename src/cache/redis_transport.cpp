#include "cpppools/cache/redis_transport.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cpppools::cache {
namespace {

int close_fd(int fd) {
#ifdef _WIN32
    return closesocket(static_cast<SOCKET>(fd));
#else
    return close(fd);
#endif
}

bool nonblocking(int fd) {
#ifdef _WIN32
    u_long value = 1;
    return ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &value) == 0;
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

std::string resp_command(const std::string& command) {
    return "*1\r\n$" + std::to_string(command.size()) + "\r\n" + command + "\r\n";
}

}  // namespace

RedisTransport::RedisTransport(std::string host, int port, RedisLimits limits)
    : host_(std::move(host)), port_(port), limits_(limits) {
    if (port_ <= 0 || port_ > 65535 || limits_.operation_timeout.count() <= 0 ||
        limits_.max_reply_bytes == 0) {
        return;
    }
#ifdef _WIN32
    static bool winsock_ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!winsock_ready) return;
#endif
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(port_);
    if (getaddrinfo(host_.c_str(), service.c_str(), &hints, &addresses) != 0) return;
    for (addrinfo* it = addresses; it != nullptr; it = it->ai_next) {
        const int fd = static_cast<int>(socket(it->ai_family, it->ai_socktype, it->ai_protocol));
        if (fd < 0 || !nonblocking(fd)) {
            if (fd >= 0) close_fd(fd);
            continue;
        }
        const int result = connect(fd, it->ai_addr, static_cast<int>(it->ai_addrlen));
        if (result == 0) {
            fd_ = fd;
            break;
        }
        close_fd(fd);
    }
    freeaddrinfo(addresses);
}

RedisTransport::~RedisTransport() { close(); }

void RedisTransport::close() {
    if (fd_ >= 0) {
        close_fd(fd_);
        fd_ = -1;
    }
}

bool RedisTransport::wait_io(bool write, std::chrono::steady_clock::time_point deadline) {
    if (fd_ < 0) return false;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) return false;
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(static_cast<SOCKET>(fd_), &set);
    timeval tv{static_cast<long>(remaining / 1000), static_cast<long>((remaining % 1000) * 1000)};
    return select(0, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv) > 0;
#else
    pollfd pfd{fd_, static_cast<short>(write ? POLLOUT : POLLIN), 0};
    return poll(&pfd, 1, static_cast<int>(remaining)) > 0;
#endif
}

bool RedisTransport::write_all(const std::string& bytes,
                               std::chrono::steady_clock::time_point deadline) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        if (!wait_io(true, deadline)) return false;
#ifdef _WIN32
        const int n = send(static_cast<SOCKET>(fd_), bytes.data() + sent,
                           static_cast<int>(bytes.size() - sent), 0);
#else
        const ssize_t n = send(fd_, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

RedisResult RedisTransport::read_reply(std::chrono::steady_clock::time_point deadline) {
    std::string reply;
    char buffer[4096];
    while (reply.size() < limits_.max_reply_bytes) {
        if (!wait_io(false, deadline)) {
            return {RedisCode::timeout, "Redis reply timeout", {}};
        }
#ifdef _WIN32
        const int n = recv(static_cast<SOCKET>(fd_), buffer, sizeof(buffer), 0);
#else
        const ssize_t n = recv(fd_, buffer, sizeof(buffer), 0);
#endif
        if (n <= 0) return {RedisCode::unavailable, "Redis connection closed", {}};
        reply.append(buffer, static_cast<std::size_t>(n));
        if (reply.find("\r\n") != std::string::npos) break;
    }
    if (reply.size() > limits_.max_reply_bytes) {
        return {RedisCode::limit_exceeded, "Redis reply exceeds configured limit", {}};
    }
    if (reply == "+PONG\r\n") return {RedisCode::ok, {}, "PONG"};
    if (!reply.empty() && reply[0] == '-') {
        return {RedisCode::protocol_error, "Redis error: " + reply, {}};
    }
    return {RedisCode::protocol_error, "invalid Redis reply", {}};
}

RedisResult RedisTransport::command(const std::string& command_text) {
    if (!valid()) return {RedisCode::unavailable, "Redis connection unavailable", {}};
    const auto deadline = std::chrono::steady_clock::now() + limits_.operation_timeout;
    if (!write_all(resp_command(command_text), deadline)) {
        close();
        return {RedisCode::unavailable, "Redis command write failed", {}};
    }
    auto result = read_reply(deadline);
    if (result.code != RedisCode::ok) close();
    return result;
}

RedisResult RedisTransport::ping() { return command("PING"); }

}  // namespace cpppools::cache
