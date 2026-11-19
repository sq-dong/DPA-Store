#ifndef POOL_ALLOCATOR_HPP
#define POOL_ALLOCATOR_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <deque>

#include "shared/server_config.h"
#include "shared/spin_lock.hpp"

class PoolAllocator {
private:
    void* buffer_;           // Pointer to the memory buffer
    std::size_t buffer_size_; // Total size of the buffer
    std::size_t chunk_size_;  // Size of each allocation chunk
    std::deque<void*> free_queue_; // FIFO queue of free chunks (addresses)
    std::size_t num_chunks_;  // Total number of chunks
    std::atomic<std::size_t> allocated_;   // Number of allocated chunks
    mutable SpinLock lock_;

public:
    // Constructor takes a pointer to memory buffer, element size, and total buffer size
    PoolAllocator(void* buffer, std::size_t element_size, std::size_t buffer_size)
        : buffer_(buffer)
        , buffer_size_(buffer_size)
        , chunk_size_(element_size)
        , num_chunks_(0)
        , allocated_(0) {
        
        assert(buffer != nullptr && "Buffer cannot be null");
        assert(element_size >= sizeof(void*) && "Element size must be at least pointer-sized");
        assert(buffer_size >= element_size && "Buffer size must be at least one element");
        assert(buffer_size % element_size == 0 && "Buffer size must be a multiple of element size");
        assert((reinterpret_cast<uintptr_t>(buffer_) & (CACHE_LINE_SIZE - 1)) == 0 &&
               "Buffer must be cache-line aligned");
        assert((chunk_size_ % CACHE_LINE_SIZE) == 0 &&
               "Chunk size must be a multiple of cache line size");
        
        initialize();
    }

    // Destructor - doesn't free the buffer (not owned by allocator)
    ~PoolAllocator() = default;

    // Allocate a single chunk
    void* allocate() {
        SpinLockGuard guard(lock_);

        if (free_queue_.empty()) {
            DOCA_LOG_ERR("ALLOCATOR FULL");
            sleep(5);
            return nullptr; // Pool is full
        }

        // Pop from the front of the queue (oldest free chunk)
        void* chunk = free_queue_.front();
        free_queue_.pop_front();
        allocated_.fetch_add(1, std::memory_order_relaxed);
        return chunk;
    }

    // Free a previously allocated chunk
    void deallocate(void* ptr) {
        if (ptr == nullptr) {
            return;
        }

        // Validate the pointer is within our buffer
        assert(ptr >= buffer_ && 
               ptr < static_cast<char*>(buffer_) + buffer_size_ &&
               "Pointer is not from this pool");
        
        // Validate alignment
        assert(((static_cast<char*>(ptr) - static_cast<char*>(buffer_)) % chunk_size_) == 0 &&
               "Pointer is not properly aligned");

        SpinLockGuard guard(lock_);

        // Append to queue tail so newest freed chunk is used last
        free_queue_.push_back(ptr);
        allocated_.fetch_sub(1, std::memory_order_relaxed);
    }

    // Reset the allocator to initial state
    void reset() {
        SpinLockGuard guard(lock_);
        allocated_.store(0, std::memory_order_relaxed);
        initialize();
    }

    // Get statistics
    std::size_t chunk_size() const { return chunk_size_; }
    std::size_t total_chunks() const { return num_chunks_; }
    std::size_t allocated_chunks() const { return allocated_.load(std::memory_order_relaxed); }
    std::size_t available_chunks() const { return num_chunks_ - allocated_chunks(); }
    bool is_full() const { return available_chunks() == 0; }
    bool is_empty() const { return allocated_chunks() == 0; }

private:
    // Initialize the free list with all chunks
    void initialize() {
        num_chunks_ = buffer_size_ / chunk_size_;
        free_queue_.clear();

        // Build queue in ascending order so fresh chunks are consumed first
        char* current = static_cast<char*>(buffer_);
        for (std::size_t i = 0; i < num_chunks_; ++i) {
            free_queue_.push_back(current);
            current += chunk_size_;
        }
    }

    // Disable copy and move
    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;
    PoolAllocator(PoolAllocator&&) = delete;
    PoolAllocator& operator=(PoolAllocator&&) = delete;
};

#endif // POOL_ALLOCATOR_HPP
