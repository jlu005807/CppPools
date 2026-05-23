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

// ObjectPool<T>：线程安全的对象复用池。
//
// 用法示例：
//   ObjectPool<Foo> pool(128);
//   auto foo = pool.acquire(...);  // 带自定义 deleter 的 unique_ptr
//   // foo 离开作用域后会自动归还到池中（RAII）。
template <typename T>
class ObjectPool {
public:
    // RAII 句柄：释放 unique_ptr 时会自动把对象放回池中。
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
        // 关键生命周期约束：
        // ObjectPool 析构前，所有已借出的句柄都必须先析构并归还。
        // 该断言可在调试阶段及时发现“池先销毁、对象后归还”的错误用法。
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
            // 锁粒度尽量小：临界区内只做池状态更新，不做对象重置。
            std::lock_guard<std::mutex> lock(mutex_);
            if (!free_list_.empty()) {
                // 优先复用空闲对象，减少动态分配。
                obj = free_list_.front();
                free_list_.pop();
            } else {
                // 池耗尽时按需扩容，保证调用方始终可获取对象。
                obj = new T();
                ++total_size_;
            }
            ++in_use_;
        }

        // 出池后重置对象内容，避免脏数据污染后续逻辑。
        // 放在锁外执行，降低锁竞争。
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
        // 通过"重建赋值"恢复对象到可用状态。
        // 对 RequestContext 这类轻量结构体非常实用，代码也更直观。
        //
        // 教学权衡：
        // - 优点：实现简单、可适配任意可拷贝/可移动赋值的 T。
        // - 缺点：若 T 中含 std::string / std::vector 等带堆资源的字段，
        //   赋值过程会先构造一个临时 T、再做移动/拷贝赋值，
        //   原对象内部已分配的 buffer 不一定能被完整复用，存在额外开销。
        // - 性能更优的写法：让 T 提供一个 reset() 成员（清零字段、
        //   保留 string/vector 的 capacity），这里改成 obj->reset()。
        //   当前项目场景下开销可忽略，因此保留这种更易读的实现。
        *obj = T(std::forward<Args>(args)...);
    }

    void release(T* obj) {
        std::lock_guard<std::mutex> lock(mutex_);
        // 归还到空闲队列，供下一次 acquire() 复用。
        free_list_.push(obj);
        assert(in_use_ > 0 && "ObjectPool in_use_ underflow");
        --in_use_;
    }

    mutable std::mutex mutex_;
    std::queue<T*> free_list_;
    std::size_t total_size_{0};
    std::size_t in_use_{0};
};
