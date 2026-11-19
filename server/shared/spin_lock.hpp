#pragma once

#include "server_config.h"

#if DPASTORE_PATCH_PATH_NEEDS_LOCKS
#include <atomic>
#include <thread>
#endif
#include <cstdint>

/* Minimal spin lock that collapses to no-ops when only one patcher is built. */
class SpinLock {
public:
    SpinLock() = default;

    void lock() {
#if DPASTORE_PATCH_PATH_NEEDS_LOCKS
        while (flag_.test_and_set(std::memory_order_acquire)) { // TODO
            std::this_thread::yield();
        }
#endif
    }

    bool try_lock() {
#if DPASTORE_PATCH_PATH_NEEDS_LOCKS
        return !flag_.test_and_set(std::memory_order_acquire);
#else
        return true;
#endif
    }

    void unlock() {
#if DPASTORE_PATCH_PATH_NEEDS_LOCKS
        //std::atomic_thread_fence(std::memory_order_seq_cst);
        flag_.clear(std::memory_order_release);
#endif
    }

    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

private:
#if DPASTORE_PATCH_PATH_NEEDS_LOCKS
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
#endif
};

class SpinLockGuard {
public:
    explicit SpinLockGuard(SpinLock &lock) : lock_(lock), owns_(true) {
        lock_.lock();
    }

    ~SpinLockGuard() {
        if (owns_) {
            //std::atomic_thread_fence(std::memory_order_seq_cst);
            lock_.unlock();
            
        }
    }

    SpinLockGuard(const SpinLockGuard&) = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;

private:
    SpinLock &lock_;
    bool owns_;
};

class RWSpinLock {
public:
    RWSpinLock() = default;

    void lock() {
#if DPASTORE_PATCH_PATH_NEEDS_LOCKS
        constexpr uint32_t writer_bit = 1u << 31;
        uint32_t expected = 0;
        while (!state_.compare_exchange_weak(expected,
                                             writer_bit,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
            expected = 0;
            std::this_thread::yield();
        }
#endif
    }

    bool try_lock() {
#if DPASTORE_PATCH_PATH_NEEDS_LOCKS
        constexpr uint32_t writer_bit = 1u << 31;
        uint32_t expected = 0;
        return state_.compare_exchange_strong(expected,
                                              writer_bit,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed);
#else
        return true;
#endif
    }

    void unlock() {
#if DPASTORE_PATCH_PATH_NEEDS_LOCKS
        state_.store(0, std::memory_order_release);
#endif
    }

    void lock_shared() {
#if DPASTORE_PATCH_PATH_NEEDS_LOCKS
        constexpr uint32_t writer_bit = 1u << 31;
        uint32_t expected = state_.load(std::memory_order_relaxed);
        while (true) {
            if (expected & writer_bit) {
                expected = state_.load(std::memory_order_relaxed);
                std::this_thread::yield();
                continue;
            }
            if (state_.compare_exchange_weak(expected,
                                             expected + 1,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
                break;
            }
        }
#endif
    }

    bool try_lock_shared() {
#if DPASTORE_PATCH_PATH_NEEDS_LOCKS
        constexpr uint32_t writer_bit = 1u << 31;
        uint32_t expected = state_.load(std::memory_order_relaxed);
        while (!(expected & writer_bit)) {
            if (state_.compare_exchange_weak(expected,
                                             expected + 1,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
#else
        return true;
#endif
    }

    void unlock_shared() {
#if DPASTORE_PATCH_PATH_NEEDS_LOCKS
        state_.fetch_sub(1, std::memory_order_release);
#endif
    }

    bool try_lock_upgrade() {
#if DPASTORE_PATCH_PATH_NEEDS_LOCKS
        constexpr uint32_t writer_bit = 1u << 31;
        uint32_t expected = 1;
        return state_.compare_exchange_strong(expected,
                                              writer_bit,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed);
#else
        return true;
#endif
    }

    RWSpinLock(const RWSpinLock&) = delete;
    RWSpinLock& operator=(const RWSpinLock&) = delete;

private:
#if DPASTORE_PATCH_PATH_NEEDS_LOCKS
    std::atomic<uint32_t> state_{0};
#endif
};

class RWSpinLockExclusiveGuard {
public:
    explicit RWSpinLockExclusiveGuard(RWSpinLock& lock) : lock_(lock), owns_(true) {
        lock_.lock();
    }

    ~RWSpinLockExclusiveGuard() {
        if (owns_) {
            lock_.unlock();
        }
    }

    RWSpinLockExclusiveGuard(const RWSpinLockExclusiveGuard&) = delete;
    RWSpinLockExclusiveGuard& operator=(const RWSpinLockExclusiveGuard&) = delete;

private:
    RWSpinLock& lock_;
    bool owns_;
};

class RWSpinLockSharedGuard {
public:
    explicit RWSpinLockSharedGuard(RWSpinLock& lock) : lock_(lock), owns_(true) {
        lock_.lock_shared();
    }

    ~RWSpinLockSharedGuard() {
        if (owns_) {
            lock_.unlock_shared();
        }
    }

    RWSpinLockSharedGuard(const RWSpinLockSharedGuard&) = delete;
    RWSpinLockSharedGuard& operator=(const RWSpinLockSharedGuard&) = delete;

private:
    RWSpinLock& lock_;
    bool owns_;
};
