#include "server/local_control.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <processthreadsapi.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace cpppools::server {
namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr std::size_t kMaxControlFileBytes = 4096;
std::atomic<bool> g_managed_stop_requested{false};
std::atomic<bool> g_stop_request_warning_reported{false};
std::atomic<unsigned long long> g_temp_sequence{0};
static_assert(std::atomic<bool>::is_always_lock_free,
              "managed signal flag must be lock-free");

void set_error(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

bool valid_launch_id(const std::string& value) {
    if (value.empty() || value.size() > 128) {
        return false;
    }
    for (char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (!(std::isalnum(c) || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

bool valid_token(const std::string& value) {
    if (value.size() < 64 || value.size() > 256 || value.size() % 2 != 0) {
        return false;
    }
    for (char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (!std::isxdigit(c)) {
            return false;
        }
    }
    return true;
}

#ifdef _WIN32
std::string windows_error(const char* operation) {
    return std::string(operation) + " failed (Windows error " +
           std::to_string(GetLastError()) + ")";
}

bool validate_directory(const fs::path& dir, std::string* error) {
    const std::wstring path = dir.wstring();
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        set_error(error, windows_error("open control directory"));
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(handle, &info) != 0;
    CloseHandle(handle);
    if (!ok) {
        set_error(error, windows_error("inspect control directory"));
        return false;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        set_error(error, "control path is not a regular directory");
        return false;
    }
    return true;
}

bool read_file_secure(const fs::path& dir, const char* filename,
                      std::string* contents, std::string* error,
                      bool missing_ok = false) {
    const fs::path path = dir / filename;
    HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (missing_ok && code == ERROR_FILE_NOT_FOUND) {
            contents->clear();
            return true;
        }
        set_error(error, "read " + std::string(filename) +
                         " failed (Windows error " + std::to_string(code) + ")");
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        CloseHandle(handle);
        set_error(error, std::string(filename) + " is not a regular file");
        return false;
    }
    const std::uint64_t size =
        (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) | info.nFileSizeLow;
    if (size > kMaxControlFileBytes) {
        CloseHandle(handle);
        set_error(error, std::string(filename) + " exceeds 4 KiB");
        return false;
    }
    contents->assign(static_cast<std::size_t>(size), '\0');
    DWORD read = 0;
    const bool read_ok = contents->empty() ||
        (ReadFile(handle, contents->data(), static_cast<DWORD>(contents->size()),
                  &read, nullptr) != 0 && read == contents->size());
    CloseHandle(handle);
    if (!read_ok) {
        set_error(error, "read " + std::string(filename) + " failed");
        return false;
    }
    return true;
}

bool write_file_atomic(const fs::path& dir, const char* filename,
                       const std::string& bytes, std::string* error) {
    if (bytes.size() > kMaxControlFileBytes) {
        set_error(error, std::string(filename) + " exceeds 4 KiB");
        return false;
    }
    const fs::path destination = dir / filename;
    const DWORD attributes = GetFileAttributesW(destination.wstring().c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) {
        set_error(error, std::string(filename) + " is not a replaceable regular file");
        return false;
    }
    const auto nonce = g_temp_sequence.fetch_add(1, std::memory_order_relaxed);
    const fs::path temporary = dir /
        (L"." + destination.filename().wstring() + L"." +
         std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(nonce) + L".tmp");
    HANDLE handle = CreateFileW(temporary.wstring().c_str(), GENERIC_WRITE, 0,
                                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        set_error(error, windows_error("create control temporary file"));
        return false;
    }
    DWORD written = 0;
    const bool write_ok = bytes.empty() ||
        (WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()),
                   &written, nullptr) != 0 && written == bytes.size());
    const bool flush_ok = write_ok && FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    if (!flush_ok) {
        DeleteFileW(temporary.wstring().c_str());
        set_error(error, windows_error("write control temporary file"));
        return false;
    }
    BOOL replace_ok = FALSE;
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        replace_ok = ReplaceFileW(destination.wstring().c_str(),
                                  temporary.wstring().c_str(), nullptr,
                                  0, nullptr, nullptr);
    } else {
        replace_ok = MoveFileExW(temporary.wstring().c_str(),
                                 destination.wstring().c_str(),
                                 MOVEFILE_WRITE_THROUGH);
    }
    if (!replace_ok) {
        DeleteFileW(temporary.wstring().c_str());
        set_error(error, windows_error("replace control file"));
        return false;
    }
    return true;
}

std::string current_process_start_time() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return {};
    }
    const std::uint64_t value =
        (static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) |
        created.dwLowDateTime;
    return std::to_string(value);
}

#else
std::string posix_error(const char* operation) {
    return std::string(operation) + " failed: " + std::strerror(errno);
}

bool open_directory(const fs::path& dir, int* fd, std::string* error) {
    struct stat st{};
    if (lstat(dir.c_str(), &st) != 0) {
        set_error(error, posix_error("inspect control directory"));
        return false;
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || st.st_uid != getuid()) {
        set_error(error, "control path must be an owned, non-symlink directory");
        return false;
    }
    *fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (*fd < 0) {
        set_error(error, posix_error("open control directory"));
        return false;
    }
    struct stat opened{};
    if (fstat(*fd, &opened) != 0 || !S_ISDIR(opened.st_mode) ||
        opened.st_uid != getuid()) {
        const int saved = errno;
        close(*fd);
        errno = saved;
        set_error(error, "control path changed or is not owned by this user");
        return false;
    }
    if (fchmod(*fd, 0700) != 0) {
        const int saved = errno;
        close(*fd);
        *fd = -1;
        errno = saved;
        set_error(error, posix_error("secure control directory"));
        return false;
    }
    return true;
}

bool read_file_secure(const fs::path& dir, const char* filename,
                      std::string* contents, std::string* error,
                      bool missing_ok = false) {
    int dir_fd = -1;
    if (!open_directory(dir, &dir_fd, error)) {
        return false;
    }
    const int fd = openat(dir_fd, filename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    const int open_error = errno;
    close(dir_fd);
    if (fd < 0) {
        if (missing_ok && open_error == ENOENT) {
            contents->clear();
            return true;
        }
        errno = open_error;
        set_error(error, posix_error((std::string("open ") + filename).c_str()));
        return false;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        set_error(error, std::string(filename) + " is not a regular file");
        return false;
    }
    if (st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) >
                               kMaxControlFileBytes) {
        close(fd);
        set_error(error, std::string(filename) + " exceeds 4 KiB");
        return false;
    }
    if ((st.st_mode & 0077) != 0) {
        close(fd);
        set_error(error, std::string(filename) + " permissions must be private");
        return false;
    }
    contents->assign(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t offset = 0;
    while (offset < contents->size()) {
        const ssize_t count = read(fd, contents->data() + offset,
                                   contents->size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            close(fd);
            set_error(error, std::string("read ") + filename + " failed");
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    close(fd);
    return true;
}

bool write_file_atomic(const fs::path& dir, const char* filename,
                       const std::string& bytes, std::string* error) {
    if (bytes.size() > kMaxControlFileBytes) {
        set_error(error, std::string(filename) + " exceeds 4 KiB");
        return false;
    }
    int dir_fd = -1;
    if (!open_directory(dir, &dir_fd, error)) {
        return false;
    }
    struct stat existing{};
    errno = 0;
    const bool destination_exists =
        fstatat(dir_fd, filename, &existing, AT_SYMLINK_NOFOLLOW) == 0;
    const int stat_error = errno;
    if (destination_exists && !S_ISREG(existing.st_mode)) {
        close(dir_fd);
        set_error(error, std::string(filename) + " is not a replaceable regular file");
        return false;
    } else if (!destination_exists && stat_error != ENOENT) {
        close(dir_fd);
        errno = stat_error;
        set_error(error, posix_error("inspect destination control file"));
        return false;
    }
    const auto nonce = g_temp_sequence.fetch_add(1, std::memory_order_relaxed);
    const std::string temporary = std::string(".") + filename + "." +
        std::to_string(static_cast<long long>(getpid())) + "." +
        std::to_string(nonce) + ".tmp";
    const int fd = openat(dir_fd, temporary.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                          0600);
    if (fd < 0) {
        close(dir_fd);
        set_error(error, posix_error("create control temporary file"));
        return false;
    }
    std::size_t offset = 0;
    bool ok = true;
    while (offset < bytes.size()) {
        const ssize_t count = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            ok = false;
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
    if (ok && fsync(fd) != 0) {
        ok = false;
    }
    const int saved_error = errno;
    close(fd);
    if (ok && renameat(dir_fd, temporary.c_str(), dir_fd, filename) != 0) {
        ok = false;
    }
    if (ok && fsync(dir_fd) != 0) {
        ok = false;
    }
    if (!ok) {
        const int failure = errno == 0 ? saved_error : errno;
        unlinkat(dir_fd, temporary.c_str(), 0);
        close(dir_fd);
        errno = failure;
        set_error(error, posix_error("write control file"));
        return false;
    }
    close(dir_fd);
    return true;
}

std::string current_process_start_time() {
    std::ifstream in("/proc/self/stat", std::ios::binary);
    std::string line;
    if (!std::getline(in, line)) {
        return {};
    }
    const auto close_name = line.rfind(')');
    if (close_name == std::string::npos || close_name + 2 >= line.size()) {
        return {};
    }
    std::istringstream fields(line.substr(close_name + 2));
    std::string value;
    for (int index = 0; index <= 19; ++index) {
        if (!(fields >> value)) {
            return {};
        }
    }
    return value;
}
#endif

bool exact_object(const Json& value,
                  std::initializer_list<std::string_view> keys) {
    if (!value.is_object() || value.size() != keys.size()) {
        return false;
    }
    for (std::string_view key : keys) {
        if (value.find(std::string(key)) == value.end()) {
            return false;
        }
    }
    return true;
}

bool valid_version(const Json& value) {
    const auto it = value.find("protocol_version");
    if (it == value.end()) {
        return false;
    }
    if (it->is_number_unsigned()) {
        return it->get<std::uint64_t>() == 1;
    }
    if (it->is_number_integer()) {
        return it->get<std::int64_t>() == 1;
    }
    return false;
}

}  // namespace

LocalControl::LocalControl(std::string dir, LocalIdentity identity,
                           std::string token)
    : dir_(std::move(dir)), identity_(std::move(identity)),
      token_(std::move(token)) {}

std::unique_ptr<LocalControl> LocalControl::open(const std::string& dir,
                                                 std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    if (dir.empty()) {
        set_error(error, "control directory is empty");
        return nullptr;
    }
    const fs::path directory(dir);
#ifdef _WIN32
    if (!validate_directory(directory, error)) {
        return nullptr;
    }
    const std::int64_t pid = static_cast<std::int64_t>(GetCurrentProcessId());
#else
    int dir_fd = -1;
    if (!open_directory(directory, &dir_fd, error)) {
        return nullptr;
    }
    close(dir_fd);
    const std::int64_t pid = static_cast<std::int64_t>(getpid());
#endif
    std::string meta_bytes;
    std::string token;
    if (!read_file_secure(directory, "control.meta", &meta_bytes, error) ||
        !read_file_secure(directory, "control.token", &token, error)) {
        return nullptr;
    }
    Json meta;
    try {
        meta = Json::parse(meta_bytes);
    } catch (const Json::exception&) {
        set_error(error, "control.meta is malformed JSON");
        return nullptr;
    }
    if (!exact_object(meta, {"protocol_version", "launch_id"}) ||
        !valid_version(meta) || !meta.at("launch_id").is_string()) {
        set_error(error, "control.meta has an invalid protocol schema");
        return nullptr;
    }
    const std::string launch_id = meta.at("launch_id").get<std::string>();
    if (!valid_launch_id(launch_id) || !valid_token(token)) {
        set_error(error, "control metadata or token is invalid");
        return nullptr;
    }
    std::string start_time = current_process_start_time();
    if (start_time.empty()) {
        set_error(error, "could not determine process start identity");
        return nullptr;
    }
    LocalIdentity identity{launch_id, pid, std::move(start_time)};
    return std::unique_ptr<LocalControl>(
        new LocalControl(directory.string(), std::move(identity), std::move(token)));
}

const LocalIdentity& LocalControl::identity() const noexcept {
    return identity_;
}

bool LocalControl::write_json(const char* filename, const void* document,
                              std::string* error) const {
    const auto& value = *static_cast<const Json*>(document);
    return write_file_atomic(fs::path(dir_), filename, value.dump() + "\n", error);
}

bool LocalControl::publish_state(const char* filename, const char* state,
                                 std::string* error) const {
    Json document{{"protocol_version", 1},
                  {"launch_id", identity_.launch_id},
                  {"pid", identity_.pid},
                  {"process_start_time", identity_.process_start_time},
                  {"state", state}};
    return write_json(filename, &document, error);
}

bool LocalControl::publish_starting(std::string* error) const {
    return publish_state("starting.json", "starting", error);
}

bool LocalControl::publish_ready(int port, const std::string& node_id,
                                 std::string* error) const {
    if (port <= 0 || port > 65535 || node_id.empty() || node_id.size() > 128) {
        set_error(error, "ready marker fields are invalid");
        return false;
    }
    Json document{{"protocol_version", 1},
                  {"launch_id", identity_.launch_id},
                  {"pid", identity_.pid},
                  {"process_start_time", identity_.process_start_time},
                  {"state", "ready"}, {"port", port}, {"node_id", node_id}};
    return write_json("ready.json", &document, error);
}

bool LocalControl::publish_stopping(std::string* error) const {
    return publish_state("stopping.json", "stopping", error);
}

bool LocalControl::publish_stopped(int exit_code, std::string* error) const {
    Json document{{"protocol_version", 1},
                  {"launch_id", identity_.launch_id},
                  {"pid", identity_.pid},
                  {"process_start_time", identity_.process_start_time},
                  {"state", "stopped"}, {"exit_kind", "graceful"},
                  {"exit_code", exit_code}};
    return write_json("stopped.json", &document, error);
}

bool LocalControl::stop_requested(std::string* error) const {
    if (error != nullptr) {
        error->clear();
    }
    std::string bytes;
    if (!read_file_secure(fs::path(dir_), "stop.request", &bytes, error, true)) {
        return false;
    }
    if (bytes.empty()) {
        return false;
    }
    Json request;
    try {
        request = Json::parse(bytes);
    } catch (const Json::exception&) {
        set_error(error, "stop.request is malformed JSON");
        return false;
    }
    if (!exact_object(request, {"protocol_version", "launch_id", "requested_at", "token"}) ||
        !valid_version(request) || !request.at("launch_id").is_string() ||
        !request.at("requested_at").is_string() ||
        !request.at("token").is_string()) {
        set_error(error, "stop.request has an invalid protocol schema");
        return false;
    }
    if (request.at("launch_id").get<std::string>() != identity_.launch_id) {
        set_error(error, "stop.request launch_id does not match");
        return false;
    }
    const std::string& requested_at = request.at("requested_at").get_ref<const std::string&>();
    if (requested_at.empty() || requested_at.size() > 64) {
        set_error(error, "stop.request requested_at is invalid");
        return false;
    }
    if (request.at("token").get<std::string>() != token_) {
        set_error(error, "stop.request token does not match");
        return false;
    }
    return true;
}

bool LocalControl::claim_stop_request_warning() const noexcept {
    return !g_stop_request_warning_reported.exchange(true, std::memory_order_relaxed);
}

void request_managed_stop() noexcept {
    g_managed_stop_requested.store(true, std::memory_order_relaxed);
}

bool managed_stop_requested() noexcept {
    return g_managed_stop_requested.load(std::memory_order_relaxed);
}

}  // namespace cpppools::server
