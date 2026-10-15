#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace cpppools::testing {

// Creates an empty, unique file in the platform temporary directory.
// Tests pass the returned path to SQLite, so the file must exist first.
inline std::string make_test_temp_file(const std::string& prefix) {
    static std::atomic<unsigned long long> sequence{0};
    const auto stamp = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto base = std::filesystem::temp_directory_path();
    for (;;) {
        const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
        const auto path = base / (prefix + "_" + std::to_string(stamp) + "_" +
                                  std::to_string(id) + ".db");
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (output.good()) {
            return path.string();
        }
    }
}

}  // namespace cpppools::testing
