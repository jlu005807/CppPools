#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace cpppools::server {

struct LocalIdentity {
    std::string launch_id;
    std::int64_t pid{0};
    std::string process_start_time;
};

class LocalControl {
public:
    static std::unique_ptr<LocalControl> open(const std::string& dir,
                                              std::string* error);

    const LocalIdentity& identity() const noexcept;
    bool publish_starting(std::string* error) const;
    bool publish_ready(int port, const std::string& node_id,
                       std::string* error) const;
    bool publish_stopping(std::string* error) const;
    bool publish_stopped(int exit_code, std::string* error) const;
    bool stop_requested(std::string* error) const;
    bool claim_stop_request_warning() const noexcept;

private:
    LocalControl(std::string dir, LocalIdentity identity, std::string token);

    bool publish_state(const char* filename, const char* state,
                       std::string* error) const;
    bool write_json(const char* filename, const void* document,
                    std::string* error) const;

    std::string dir_;  // UTF-8, including on Windows.
    LocalIdentity identity_;
    std::string token_;
};

// The managed signal handler is deliberately limited to this lock-free flag.
void request_managed_stop() noexcept;
bool managed_stop_requested() noexcept;

}  // namespace cpppools::server
