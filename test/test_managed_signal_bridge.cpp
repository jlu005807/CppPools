#define main cpppools_embedded_server_main
#include "../src/main.cpp"
#undef main

#include <cassert>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <thread>

namespace {

struct DestructorGate {
    std::mutex& mutex;
    std::condition_variable& changed;
    bool& entered;
    bool& released;

    ~DestructorGate() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [this] { return released; });
    }
};

}  // namespace

int main() {
    std::mutex mutex;
    std::condition_variable changed;
    bool destructor_entered = false;
    bool release_destructor = false;

    ManagedStopBridge managed_bridge;
    std::thread signal_sender([&] {
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return destructor_entered; });
        }
        std::raise(SIGTERM);
        std::raise(SIGTERM);
        {
            std::lock_guard<std::mutex> lock(mutex);
            release_destructor = true;
        }
        changed.notify_all();
    });

    {
        DestructorGate resource{mutex, changed, destructor_entered,
                                release_destructor};
    }
    signal_sender.join();
    assert(managed_stop_requested());
    return 0;
}
