#include "server/local_control.h"

#include <nlohmann/json.hpp>

#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <aclapi.h>
#include <winioctl.h>
#else
#include <unistd.h>
#endif

namespace {
using cpppools::server::LocalControl;
using nlohmann::json;
namespace fs = std::filesystem;
std::atomic<unsigned long long> g_temp_sequence{0};

struct TempDir {
    fs::path path;

    TempDir() {
#ifdef _WIN32
        const auto pid = _getpid();
#else
        const auto pid = getpid();
#endif
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            path = fs::temp_directory_path() /
                   ("cpppools_local_control_" + std::to_string(pid) + "_" +
                    std::to_string(stamp) + "_" +
                    std::to_string(g_temp_sequence.fetch_add(1)));
            std::error_code ec;
            if (fs::create_directory(path, ec)) {
                return;
            }
            assert(ec == std::errc::file_exists);
        }
        assert(false && "could not allocate a unique local-control test directory");
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

constexpr const char* kLaunchId = "launch-for-test";
constexpr const char* kToken =
    "7e2f3a1b4c5d6e7f8a9b0c1d2e3f4051"
    "7e2f3a1b4c5d6e7f8a9b0c1d2e3f4051";

void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    assert(out.good());
    out.close();
#ifndef _WIN32
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
#endif
}

void write_meta(const fs::path& dir, json value = {
    {"protocol_version", 1}, {"launch_id", kLaunchId}}) {
    write_file(dir / "control.meta", value.dump());
    write_file(dir / "control.token", kToken);
}

void write_stop_request(const fs::path& dir, json value = {
    {"protocol_version", 1}, {"launch_id", kLaunchId},
    {"requested_at", "2026-10-14T00:00:00Z"}, {"token", kToken}}) {
    write_file(dir / "stop.request", value.dump());
}

bool opens(const fs::path& dir) {
    std::string error;
    return LocalControl::open(dir.string(), &error) != nullptr;
}

void test_meta_requires_v1_and_launch_id() {
    TempDir temp;
    write_meta(temp.path, {{"protocol_version", 2}, {"launch_id", kLaunchId}});
    assert(!opens(temp.path));

    write_meta(temp.path, {{"protocol_version", 4294967297LL},
                           {"launch_id", kLaunchId}});
    assert(!opens(temp.path));

    write_meta(temp.path, {{"protocol_version", -1}, {"launch_id", kLaunchId}});
    assert(!opens(temp.path));

    write_meta(temp.path, {{"protocol_version", 1.0}, {"launch_id", kLaunchId}});
    assert(!opens(temp.path));

    write_meta(temp.path, {{"protocol_version", 1}, {"launch_id", 7}});
    assert(!opens(temp.path));

    write_meta(temp.path, {{"protocol_version", 1}, {"launch_id", ""}});
    assert(!opens(temp.path));
}

void test_corrupt_and_oversize_metadata_are_rejected() {
    TempDir temp;
    write_file(temp.path / "control.meta", "{not-json");
    write_file(temp.path / "control.token", kToken);
    assert(!opens(temp.path));

    write_file(temp.path / "control.meta", std::string(4097, 'x'));
    assert(!opens(temp.path));
}

void test_symlink_metadata_is_rejected() {
#ifdef _WIN32
    TempDir temp;
    write_meta(temp.path);
    const fs::path actual = temp.path / "actual.meta";
    fs::rename(temp.path / "control.meta", actual);
    const fs::path link = temp.path / "control.meta";
    const DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (!CreateSymbolicLinkW(link.c_str(), actual.c_str(), flags)) {
        const DWORD error = GetLastError();
        if (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_ACCESS_DENIED ||
            error == ERROR_INVALID_PARAMETER) {
            // An unknown GUID tag is an actual file-level reparse point and
            // needs no symlink privilege. Both use the same rejection branch.
            write_file(link, "");
            HANDLE file = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            assert(file != INVALID_HANDLE_VALUE);
            struct ReparseData {
                DWORD tag;
                WORD length;
                WORD reserved;
                GUID guid;
            } data{0x42, 0, 0,
                   {0xaaac7164, 0xcf2b, 0x4d13, {0xae, 0x67, 0x81, 0xbe, 0x63, 0x0a, 0xd3, 0xe0}}};
            DWORD returned = 0;
            assert(DeviceIoControl(file, FSCTL_SET_REPARSE_POINT, &data, sizeof(data),
                                    nullptr, 0, &returned, nullptr));
            CloseHandle(file);
            assert(GetFileAttributesW(link.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT);
            std::cout << "Windows file-level GUID reparse fixture exercised\n";
        } else {
            assert(false && "CreateSymbolicLinkW failed unexpectedly");
        }
    }
    assert(!opens(temp.path));
#else
    TempDir temp;
    write_meta(temp.path);
    const fs::path actual = temp.path / "actual.meta";
    fs::rename(temp.path / "control.meta", actual);
    fs::create_symlink(actual.filename(), temp.path / "control.meta");
    assert(!opens(temp.path));
#endif
}

void test_windows_control_directory_is_private() {
#ifdef _WIN32
    TempDir temp;
    write_meta(temp.path);
    std::string error;
    auto control = LocalControl::open(temp.path.string(), &error);
    if (!control) { std::cerr << "ACL open failed: " << error << "\n"; }
    assert(control);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL acl = nullptr;
    assert(GetNamedSecurityInfoW(const_cast<wchar_t*>(temp.path.c_str()), SE_FILE_OBJECT,
                                 DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl,
                                 nullptr, &descriptor) == ERROR_SUCCESS);
    SECURITY_DESCRIPTOR_CONTROL flags = 0;
    DWORD revision = 0;
    assert(GetSecurityDescriptorControl(descriptor, &flags, &revision));
    assert(flags & SE_DACL_PROTECTED);
    assert(acl && acl->AceCount == 2);
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void* raw = nullptr;
        assert(GetAce(acl, index, &raw));
        const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
        assert(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE);
        assert((ace->Header.AceFlags & INHERITED_ACE) == 0);
        assert(ace->Mask == FILE_ALL_ACCESS);
    }
    LocalFree(descriptor);
#endif
}

void test_stop_request_is_authenticated_versioned_and_repeatable() {
    TempDir temp;
    write_meta(temp.path);
    std::string error;
    auto control = LocalControl::open(temp.path.string(), &error);
    assert(control && error.empty());

    write_stop_request(temp.path, {{"protocol_version", 1},
                                   {"launch_id", "another-launch"},
                                   {"requested_at", "now"}, {"token", kToken}});
    assert(!control->stop_requested(&error));
    assert(error == "stop.request launch_id does not match");
    assert(error.find(kToken) == std::string::npos);

    write_stop_request(temp.path);
    error.clear();
    assert(control->stop_requested(&error));
    assert(control->stop_requested(&error));

    const std::string invalid_token = "invalid-token-must-not-be-logged";
    write_stop_request(temp.path, {{"protocol_version", 1},
                                   {"launch_id", kLaunchId},
                                   {"requested_at", "now"}, {"token", invalid_token}});
    error.clear();
    assert(!control->stop_requested(&error));
    assert(error == "stop.request token does not match");
    assert(error.find(invalid_token) == std::string::npos);
    auto second_control = LocalControl::open(temp.path.string(), &error);
    assert(second_control && error.empty());
    assert(control->claim_stop_request_warning());
    assert(!second_control->claim_stop_request_warning());

    write_file(temp.path / "stop.request", std::string(4097, 'x'));
    assert(!control->stop_requested(&error));
}

void test_marker_replace_failure_is_reported() {
    TempDir temp;
    write_meta(temp.path);
    std::string error;
    auto control = LocalControl::open(temp.path.string(), &error);
    assert(control);
    fs::create_directory(temp.path / "starting.json");
    assert(!control->publish_starting(&error));
    assert(!error.empty());
}

void test_existing_marker_is_atomically_replaced() {
    TempDir temp;
    write_meta(temp.path);
    std::string error;
    auto control = LocalControl::open(temp.path.string(), &error);
    assert(control && error.empty());

    assert(control->publish_ready(8123, "node-before", &error));
    assert(control->publish_ready(8124, "node-after", &error));

    std::ifstream in(temp.path / "ready.json", std::ios::binary);
    const json ready = json::parse(in);
    assert(ready.at("port").get<int>() == 8124);
    assert(ready.at("node_id").get<std::string>() == "node-after");
}

void test_marker_schema_and_managed_stop_flag() {
    TempDir temp;
    write_meta(temp.path);
    std::string error;
    auto control = LocalControl::open(temp.path.string(), &error);
    assert(control);
    assert(control->publish_starting(&error));
    assert(control->publish_ready(8123, "node-a", &error));
    assert(control->publish_stopping(&error));
    assert(control->publish_stopped(0, &error));

    std::ifstream in(temp.path / "stopped.json", std::ios::binary);
    const json stopped = json::parse(in);
    assert(stopped.at("protocol_version").get<int>() == 1);
    assert(stopped.at("launch_id").get<std::string>() == kLaunchId);
    assert(stopped.at("pid").get<std::int64_t>() == control->identity().pid);
    assert(stopped.at("process_start_time").get<std::string>() ==
           control->identity().process_start_time);
    assert(stopped.at("state").get<std::string>() == "stopped");
    assert(stopped.at("exit_kind").get<std::string>() == "graceful");
    assert(stopped.at("exit_code").get<int>() == 0);

    assert(!cpppools::server::managed_stop_requested());
    cpppools::server::request_managed_stop();
    cpppools::server::request_managed_stop();
    assert(cpppools::server::managed_stop_requested());
}
}  // namespace

int main() {
    test_windows_control_directory_is_private();
    test_meta_requires_v1_and_launch_id();
    test_corrupt_and_oversize_metadata_are_rejected();
    test_symlink_metadata_is_rejected();
    test_stop_request_is_authenticated_versioned_and_repeatable();
    test_marker_replace_failure_is_reported();
    test_existing_marker_is_atomically_replaced();
    test_marker_schema_and_managed_stop_flag();
    return 0;
}
