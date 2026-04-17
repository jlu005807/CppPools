#pragma once

#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <type_traits>
#include <utility>

// ObjectPool<T>: simple thread-safe reusable object storage.
//
// Usage:
//   ObjectPool<Foo> pool(128);
//   auto foo = pool.acquire(...);  // foo is unique_ptr with custom deleter
//   // when foo goes out of scope, it is returned to pool automatically.
template <typename T>
class ObjectPool {
public:
    // RAII handle. Releasing this pointer returns object back to pool.
    using Ptr = std::unique_ptr<T, std::function<void(T*)>>;

    explicit ObjectPool(std::size_t init_size) {
        if (init_size == 0) {
            throw std::invalid_argument("ObjectPool init_size must be > 0");
        }
        for (std::size_t i = 0; i < init_size; ++i) {
            free_list_.push(new T());
        }
        total_size_ = init_size;
    }

    ~ObjectPool() {
        std::lock_guard<std::mutex> lock(mutex_);
        // Important lifetime contract:
        // all acquired handles must be destroyed before ObjectPool itself.
        // In debug builds this assert catches accidental dangling handles.
        assert(in_use_ == 0 && "ObjectPool destroyed while objects are still in use");
        while (!free_list_.empty()) {
            delete free_list_.front();
            free_list_.pop();
        }
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template <typename... Args>
    Ptr acquire(Args&&... args) {
        T* obj = nullptr;
        {
            // Keep lock scope minimal: only manipulate pool bookkeeping here.
            std::lock_guard<std::mutex> lock(mutex_);
            if (!free_list_.empty()) {
                // Reuse an existing object first.
                obj = free_list_.front();
                free_list_.pop();
            } else {
                // Grow on demand if pool exhausted.
                obj = new T();
                ++total_size_;
            }
            ++in_use_;
        }

        // Reinitialize content for caller to avoid stale state.
        // This happens outside lock to reduce contention.
        reset_object(obj, std::forward<Args>(args)...);
        return Ptr(obj, [this](T* p) { this->release(p); });
    }

    std::size_t in_use() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return in_use_;
    }

    std::size_t available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return free_list_.size();
    }

    std::size_t total_size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_size_;
    }

private:
    template <typename... Args>
    static void reset_object(T* obj, Args&&... args) {
        // Assign a reconstructed value. This keeps implementation simple and
        // works well for request/context-like structs.
        *obj = T(std::forward<Args>(args)...);
    }

    void release(T* obj) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Return object into free list for next acquire().
        free_list_.push(obj);
        assert(in_use_ > 0 && "ObjectPool in_use_ underflow");
        --in_use_;
    }

    mutable std::mutex mutex_;
    std::queue<T*> free_list_;
    std::size_t total_size_{0};
    std::size_t in_use_{0};
};
