#include "cpppools/cache/redis_transport.h"

#include <cerrno>
#include <functional>
#include <optional>
#include <utility>

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

int close_fd(std::intptr_t fd) {
#ifdef _WIN32
    return closesocket(static_cast<SOCKET>(fd));
#else
    return close(static_cast<int>(fd));
#endif
}

bool invalid_fd(std::intptr_t fd) {
#ifdef _WIN32
    return fd == static_cast<std::intptr_t>(INVALID_SOCKET);
#else
    return fd < 0;
#endif
}

bool nonblocking(std::intptr_t fd) {
#ifdef _WIN32
    u_long value = 1;
    return ioctlsocket(static_cast<SOCKET>(fd), static_cast<long>(FIONBIO), &value) == 0;
#else
    const int native_fd = static_cast<int>(fd);
    const int flags = fcntl(native_fd, F_GETFL, 0);
    return flags >= 0 && fcntl(native_fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

std::string resp_command(const std::vector<std::string>& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& arg : args) {
        out += "$" + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
    }
    return out;
}

bool read_exact(std::intptr_t fd, char* data, std::size_t size,
                const std::function<bool()>& ready) {
    std::size_t offset = 0;
    while (offset < size) {
        if (!ready()) return false;
#ifdef _WIN32
        const int count = recv(static_cast<SOCKET>(fd), data + offset,
                               static_cast<int>(size - offset), 0);
#else
        const ssize_t count = recv(static_cast<int>(fd), data + offset, size - offset, 0);
#endif
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
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
        const std::intptr_t fd = static_cast<std::intptr_t>(
            socket(it->ai_family, it->ai_socktype, it->ai_protocol));
        if (invalid_fd(fd) || !nonblocking(fd)) {
            if (!invalid_fd(fd)) close_fd(fd);
            continue;
        }
        int result = 0;
#ifdef _WIN32
        result = connect(static_cast<SOCKET>(fd), it->ai_addr,
                         static_cast<int>(it->ai_addrlen));
#else
        result = connect(static_cast<int>(fd), it->ai_addr,
                         static_cast<int>(it->ai_addrlen));
#endif
        bool connected = result == 0;
        if (!connected) {
#ifdef _WIN32
            const int error = WSAGetLastError();
            connected = error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
            connected = errno == EINPROGRESS;
#endif
            if (connected) {
                fd_ = fd;
                const auto deadline = std::chrono::steady_clock::now() + limits_.operation_timeout;
                if (wait_io(true, deadline)) {
                    int status = 0;
                    socklen_t length = sizeof(status);
#ifdef _WIN32
                    connected = getsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_ERROR,
                                           reinterpret_cast<char*>(&status), &length) == 0 &&
#else
                    connected = getsockopt(static_cast<int>(fd), SOL_SOCKET, SO_ERROR,
                                           reinterpret_cast<char*>(&status), &length) == 0 &&
#endif
                                status == 0;
                } else {
                    connected = false;
                }
                if (!connected) fd_ = -1;
            }
        }
        if (connected) {
            fd_ = fd;
            break;
        }
        close_fd(fd);
    }
    freeaddrinfo(addresses);
}

RedisTransport::~RedisTransport() { close(); }

void RedisTransport::close() {
    if (fd_ != -1) {
        close_fd(fd_);
        fd_ = -1;
    }
}

bool RedisTransport::wait_io(bool write, std::chrono::steady_clock::time_point deadline) {
    if (fd_ == -1) return false;
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
    pollfd pfd{static_cast<int>(fd_), static_cast<short>(write ? POLLOUT : POLLIN), 0};
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
        const ssize_t n = send(static_cast<int>(fd_), bytes.data() + sent,
                               bytes.size() - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

RedisResult RedisTransport::read_reply(std::chrono::steady_clock::time_point deadline) {
    auto ready = [&] { return wait_io(false, deadline); };
    auto line = [&]() -> std::optional<std::string> {
        std::string result;
        char byte = 0;
        while (result.size() <= limits_.max_reply_bytes) {
            if (!read_exact(fd_, &byte, 1, ready)) return std::nullopt;
            if (byte == '\r') {
                if (!read_exact(fd_, &byte, 1, ready) || byte != '\n') return std::nullopt;
                return result;
            }
            result.push_back(byte);
        }
        return std::nullopt;
    };
    char prefix = 0;
    if (!read_exact(fd_, &prefix, 1, ready)) {
        return {RedisCode::timeout, "Redis reply timeout", {}};
    }
    const auto header = line();
    if (!header) return {RedisCode::protocol_error, "invalid Redis reply", {}};
    if (prefix == '+') return {RedisCode::ok, {}, *header};
    if (prefix == '-') return {RedisCode::protocol_error, "Redis error: " + *header, {}};
    if (prefix == ':') return {RedisCode::ok, {}, *header};
    if (prefix != '$') return {RedisCode::protocol_error, "invalid Redis reply", {}};
    long long length = 0;
    try {
        length = std::stoll(*header);
    } catch (...) {
        return {RedisCode::protocol_error, "invalid Redis bulk length", {}};
    }
    if (length == -1) return {RedisCode::not_found, {}, {}};
    if (length < 0 || static_cast<std::size_t>(length) > limits_.max_reply_bytes) {
        return {RedisCode::limit_exceeded, "Redis reply exceeds configured limit", {}};
    }
    std::string value(static_cast<std::size_t>(length), '\0');
    if (!read_exact(fd_, value.data(), value.size(), ready)) {
        return {RedisCode::timeout, "Redis reply timeout", {}};
    }
    char crlf[2]{};
    if (!read_exact(fd_, crlf, sizeof(crlf), ready) || crlf[0] != '\r' || crlf[1] != '\n') {
        return {RedisCode::protocol_error, "invalid Redis bulk reply", {}};
    }
    return {RedisCode::ok, {}, std::move(value)};
}

RedisResult RedisTransport::command(const std::string& command_text) {
    return command(std::vector<std::string>{command_text});
}

RedisResult RedisTransport::command(const std::vector<std::string>& args) {
    if (!valid()) return {RedisCode::unavailable, "Redis connection unavailable", {}};
    const auto deadline = std::chrono::steady_clock::now() + limits_.operation_timeout;
    if (!write_all(resp_command(args), deadline)) {
        close();
        return {RedisCode::unavailable, "Redis command write failed", {}};
    }
    auto result = read_reply(deadline);
    if (result.code != RedisCode::ok && result.code != RedisCode::not_found) close();
    return result;
}

RedisResult RedisTransport::ping() { return command("PING"); }

RedisResult RedisTransport::get(const std::string& key) {
    return command(std::vector<std::string>{"GET", key});
}

RedisResult RedisTransport::set_px(const std::string& key, const std::string& value,
                                   std::chrono::milliseconds ttl) {
    if (ttl.count() <= 0) return {RedisCode::protocol_error, "Redis TTL must be positive", {}};
    return command(std::vector<std::string>{"SET", key, value, "PX",
                                            std::to_string(ttl.count())});
}

RedisResult RedisTransport::del(const std::string& key) {
    return command(std::vector<std::string>{"DEL", key});
}

}  // namespace cpppools::cache
