#ifndef DPATREE_ALLOCATOR_HPP
#define DPATREE_ALLOCATOR_HPP

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <vector>
#include <deque>
#include "../shared/server_config.h"
#include "../shared/spin_lock.hpp"

/* Host-side shadow allocator for DPA tree memory pools
 * 
 * PURPOSE:
 * Pre-computes DPA addresses without touching actual DPA memory during tree construction.
 * The host builds the complete learned tree structure in host memory, then sends "stitch" 
 * commands to DPA with pre-computed DPA addresses for zero-copy tree replication.
 * 
 * WHY MULTIPLE SEGMENTS:
 * DPA heap allocation (flexio_buf_dev_alloc) has size limitations per allocation call.
 * To access the full 16GB DPA memory, we need multiple separate allocation calls, which
 * results in non-contiguous memory ranges. Each allocation is limited in size, so we
 * allocate multiple segments of equal size (e.g., 16 segments × 1GB = 16GB total).
 * 
 * HOW IT WORKS:
 * 1. Virtual Address Space: Uses 0-based virtual addressing for pools (leaf + inner)
 * 2. Segment Translation: virtual_to_device() maps virtual addresses to actual DPA addresses
 * 3. Boundary Checking: Ensures no object spans across segment boundaries  
 * 4. Segment Jumping: Moves to next segment if allocation would cross boundary (wastes small space)
 * 5. Cache Alignment: All pools start at cache-line aligned offsets for optimal performance
 * 
 * EXAMPLE:
 * Virtual addr 0x50000 with 10MB segments -> segment 5, offset 0x0000 -> maps to dpa_segments[5] + 0x0000
 * If allocation of 8KB would cross segment boundary, jump to next segment start
 */

struct DeviceAllocation {
    void* device_addr;              // Actual device address to use
    uintptr_t adjusted_virtual_addr; // Virtual address after potential segment jump
    bool success;                   // Whether allocation succeeded
};

struct AllocationInfo {
    void* device_addr;
    uintptr_t virtual_addr;
};

class DPATreeAllocator {
public:
    DPATreeAllocator(const std::vector<void*>& dpa_ranges, size_t range_size)
        : range_size_(range_size)
        , dpa_ranges_(dpa_ranges)
        , leaf_offset_(0)
        , inner_offset_(0) {
        // Calculate cache-aligned pool base addresses
        size_t base_offset = 1024;
        size_t aligned_offset = align_to_cache_line(base_offset);
        
        virtual_leaf_pool_base_ = aligned_offset;
        virtual_inner_pool_base_ = align_to_cache_line(virtual_leaf_pool_base_ + DPA_TREE_LEAF_POOL_SIZE);
    }
    
    // // Allocate a leaf node and return its DPA address
    // void* allocate_leaf(void* host_addr) {
    //     SpinLockGuard guard(lock_);
    //     uintptr_t virtual_addr = 0;
    //     bool fresh_available = (leaf_offset_ + DPA_LEAF_NODE_SIZE) <= DPA_TREE_LEAF_POOL_SIZE;

    //     if (fresh_available) {
    //         virtual_addr = virtual_leaf_pool_base_ + leaf_offset_;
    //         auto alloc = virtual_to_device(virtual_addr, DPA_LEAF_NODE_SIZE);
    //         if (!alloc.success) {
    //             // Fall back to reused offsets if mapping fails
    //             if (free_leaf_offsets_.empty()) {
    //                 return nullptr;
    //             }
    //             virtual_addr = virtual_leaf_pool_base_ + free_leaf_offsets_.front();
    //             free_leaf_offsets_.pop_front();
    //             alloc = virtual_to_device(virtual_addr, DPA_LEAF_NODE_SIZE);
    //             if (!alloc.success) {
    //                 free_leaf_offsets_.push_back(virtual_addr - virtual_leaf_pool_base_);
    //                 return nullptr;
    //             }

    //             leaf_allocations_[host_addr] = {alloc.device_addr, alloc.adjusted_virtual_addr};
    //             return alloc.device_addr;
    //         }

    //         // Update offset if we jumped to next segment
    //         if (alloc.adjusted_virtual_addr != virtual_addr) {
    //             leaf_offset_ = alloc.adjusted_virtual_addr - virtual_leaf_pool_base_;
    //         }
    //         leaf_offset_ = (alloc.adjusted_virtual_addr - virtual_leaf_pool_base_) + DPA_LEAF_NODE_SIZE;

    //         leaf_allocations_[host_addr] = {alloc.device_addr, alloc.adjusted_virtual_addr};
    //         return alloc.device_addr;
    //     }

    //     if (free_leaf_offsets_.empty()) {
    //         return nullptr;  // Pool exhausted
    //     }

    //     virtual_addr = virtual_leaf_pool_base_ + free_leaf_offsets_.front();
    //     free_leaf_offsets_.pop_front();
    //     auto alloc = virtual_to_device(virtual_addr, DPA_LEAF_NODE_SIZE);
    //     if (!alloc.success) {
    //         free_leaf_offsets_.push_back(virtual_addr - virtual_leaf_pool_base_);
    //         return nullptr;
    //     }
        
    //     // Record mappings
    //     leaf_allocations_[host_addr] = {alloc.device_addr, alloc.adjusted_virtual_addr};
        
    //     return alloc.device_addr;
    // }
    
    // // Allocate an inner node and return its DPA address
    // void* allocate_inner(void* host_addr) {
    //     SpinLockGuard guard(lock_);
    //     uintptr_t virtual_addr = 0;
    //     bool fresh_available = (inner_offset_ + DPA_INNER_NODE_SIZE) <= DPA_TREE_INNER_POOL_SIZE;

    //     if (fresh_available) {
    //         virtual_addr = virtual_inner_pool_base_ + inner_offset_;
    //         auto alloc = virtual_to_device(virtual_addr, DPA_INNER_NODE_SIZE);
    //         if (!alloc.success) {
    //             if (free_inner_offsets_.empty()) {
    //                 return nullptr;
    //             }
    //             virtual_addr = virtual_inner_pool_base_ + free_inner_offsets_.front();
    //             free_inner_offsets_.pop_front();
    //             alloc = virtual_to_device(virtual_addr, DPA_INNER_NODE_SIZE);
    //             if (!alloc.success) {
    //                 free_inner_offsets_.push_back(virtual_addr - virtual_inner_pool_base_);
    //                 return nullptr;
    //             }

    //             inner_allocations_[host_addr] = {alloc.device_addr, alloc.adjusted_virtual_addr};
    //             return alloc.device_addr;
    //         }

    //         if (alloc.adjusted_virtual_addr != virtual_addr) {
    //             inner_offset_ = alloc.adjusted_virtual_addr - virtual_inner_pool_base_;
    //         }
    //         inner_offset_ = (alloc.adjusted_virtual_addr - virtual_inner_pool_base_) + DPA_INNER_NODE_SIZE;

    //         inner_allocations_[host_addr] = {alloc.device_addr, alloc.adjusted_virtual_addr};
    //         return alloc.device_addr;
    //     }

    //     if (free_inner_offsets_.empty()) {
    //         return nullptr;  // Pool exhausted
    //     }

    //     virtual_addr = virtual_inner_pool_base_ + free_inner_offsets_.front();
    //     free_inner_offsets_.pop_front();
    //     auto alloc = virtual_to_device(virtual_addr, DPA_INNER_NODE_SIZE);
    //     if (!alloc.success) {
    //         free_inner_offsets_.push_back(virtual_addr - virtual_inner_pool_base_);
    //         return nullptr;
    //     }
        
    //     // Record mappings
    //     inner_allocations_[host_addr] = {alloc.device_addr, alloc.adjusted_virtual_addr};
        
    //     return alloc.device_addr;
    // }
    
    // Allocate a learned leaf node and return its DPA address
    void* allocate_learned_leaf(void* host_addr) {
        SpinLockGuard guard(lock_);
        void* ret = allocate_learned_leaf_locked(host_addr);
        return ret;
    }

    void* ensure_learned_leaf(void* host_addr) {
        SpinLockGuard guard(lock_);
        auto existing = leaf_allocations_.find(host_addr);
        if (existing != leaf_allocations_.end()) {
            return existing->second.device_addr;
        }
        void* ret = allocate_learned_leaf_locked(host_addr);
        return ret;
    }

    void* allocate_learned_inner(void* host_addr) {
        SpinLockGuard guard(lock_);
        void* ret = allocate_learned_inner_locked(host_addr);
        return ret;
    }

    void* ensure_learned_inner(void* host_addr) {
        SpinLockGuard guard(lock_);
        auto existing = inner_allocations_.find(host_addr);
        if (existing != inner_allocations_.end()) {
            return existing->second.device_addr;
        }
        void* ret = allocate_learned_inner_locked(host_addr);
        return ret;
    }

private:
    void* allocate_learned_leaf_locked(void* host_addr) {
        uintptr_t virtual_addr = 0;
        bool fresh_available = (leaf_offset_ + DPA_LEAF_NODE_SIZE) <= DPA_TREE_LEAF_POOL_SIZE;

        if (fresh_available) {
            virtual_addr = virtual_leaf_pool_base_ + leaf_offset_;
            auto alloc = virtual_to_device(virtual_addr, DPA_LEAF_NODE_SIZE);
            if (!alloc.success) {
                if (free_leaf_offsets_.empty()) {
                    return nullptr;
                }
                virtual_addr = virtual_leaf_pool_base_ + free_leaf_offsets_.front();
                free_leaf_offsets_.pop_front();
                alloc = virtual_to_device(virtual_addr, DPA_LEAF_NODE_SIZE);
                if (!alloc.success) {
                    free_leaf_offsets_.push_back(virtual_addr - virtual_leaf_pool_base_);
                    return nullptr;
                }

                leaf_allocations_[host_addr] = {alloc.device_addr, alloc.adjusted_virtual_addr};
                return alloc.device_addr;
            }

            if (alloc.adjusted_virtual_addr != virtual_addr) {
                leaf_offset_ = alloc.adjusted_virtual_addr - virtual_leaf_pool_base_;
            }
            leaf_offset_ = (alloc.adjusted_virtual_addr - virtual_leaf_pool_base_) + DPA_LEAF_NODE_SIZE;

            leaf_allocations_[host_addr] = {alloc.device_addr, alloc.adjusted_virtual_addr};
            return alloc.device_addr;
        }

        if (free_leaf_offsets_.empty()) {
            return nullptr;  // Pool exhausted
        }

        virtual_addr = virtual_leaf_pool_base_ + free_leaf_offsets_.front();
        free_leaf_offsets_.pop_front();
        auto alloc = virtual_to_device(virtual_addr, DPA_LEAF_NODE_SIZE);
        if (!alloc.success) {
            free_leaf_offsets_.push_back(virtual_addr - virtual_leaf_pool_base_);
            return nullptr;
        }
        
        // Record mappings
        leaf_allocations_[host_addr] = {alloc.device_addr, alloc.adjusted_virtual_addr};
        
        return alloc.device_addr;
    }
    
    // Allocate a learned inner node and return its DPA address  
    void* allocate_learned_inner_locked(void* host_addr) {
        uintptr_t virtual_addr = 0;
        bool fresh_available = (inner_offset_ + DPA_INNER_NODE_SIZE) <= DPA_TREE_INNER_POOL_SIZE;

        if (fresh_available) {
            virtual_addr = virtual_inner_pool_base_ + inner_offset_;
            auto alloc = virtual_to_device(virtual_addr, DPA_INNER_NODE_SIZE);
            if (!alloc.success) {
                DOCA_LOG_ERR("NO AVAILABLE RANGES");
                if (free_inner_offsets_.empty()) {
                    return nullptr;
                }
                virtual_addr = virtual_inner_pool_base_ + free_inner_offsets_.front();
                free_inner_offsets_.pop_front();
                alloc = virtual_to_device(virtual_addr, DPA_INNER_NODE_SIZE);
                if (!alloc.success) {
                    DOCA_LOG_ERR("NO AVAILABLE RANGES");
                    free_inner_offsets_.push_back(virtual_addr - virtual_inner_pool_base_);
                    return nullptr;
                }

                inner_allocations_[host_addr] = {alloc.device_addr, alloc.adjusted_virtual_addr};
                return alloc.device_addr;
            }

            if (alloc.adjusted_virtual_addr != virtual_addr) {
                inner_offset_ = alloc.adjusted_virtual_addr - virtual_inner_pool_base_;
            }
            inner_offset_ = (alloc.adjusted_virtual_addr - virtual_inner_pool_base_) + DPA_INNER_NODE_SIZE;

            inner_allocations_[host_addr] = {alloc.device_addr, alloc.adjusted_virtual_addr};
            return alloc.device_addr;
        }

        if (free_inner_offsets_.empty()) {
            DOCA_LOG_ERR("POOL EXHAUSTED");
            return nullptr;
        }

        virtual_addr = virtual_inner_pool_base_ + free_inner_offsets_.front();
        free_inner_offsets_.pop_front();
        auto alloc = virtual_to_device(virtual_addr, DPA_INNER_NODE_SIZE);
        if (!alloc.success) {
            DOCA_LOG_ERR("NO AVAILABLE RANGES");
            free_inner_offsets_.push_back(virtual_addr - virtual_inner_pool_base_);
            return nullptr;
        }
        
        // Record mappings
        inner_allocations_[host_addr] = {alloc.device_addr, alloc.adjusted_virtual_addr};
        
        return alloc.device_addr;
    }

public:
    // Translate host address to DPA address
    void* translate(void* host_addr) const {
        SpinLockGuard guard(lock_);
        auto leaf_it = leaf_allocations_.find(host_addr);
        if (leaf_it != leaf_allocations_.end()) {
            return leaf_it->second.device_addr;
        }
        auto inner_it = inner_allocations_.find(host_addr);
        if (inner_it != inner_allocations_.end()) {
            return inner_it->second.device_addr;
        }
        return nullptr;
    }

    bool release_learned_leaf(void* host_addr) {
        SpinLockGuard guard(lock_);
        auto it = leaf_allocations_.find(host_addr);
        if (it == leaf_allocations_.end()) {
            return false;
        }
        free_leaf_offsets_.push_back(it->second.virtual_addr - virtual_leaf_pool_base_);
        leaf_allocations_.erase(it);
        return true;
    }

    bool release_learned_inner(void* host_addr) {
        SpinLockGuard guard(lock_);
        auto it = inner_allocations_.find(host_addr);
        if (it == inner_allocations_.end()) {
            return false;
        }
        free_inner_offsets_.push_back(it->second.virtual_addr - virtual_inner_pool_base_);
        inner_allocations_.erase(it);
        return true;
    }
    
    
    // // Get base addresses for initialization
    // void* get_leaf_pool_base() const { 
    //     SpinLockGuard guard(lock_);
    //     auto alloc = virtual_to_device(virtual_leaf_pool_base_, 1);
    //     return alloc.success ? alloc.device_addr : nullptr;
    // }

    // void* get_inner_pool_base() const { 
    //     SpinLockGuard guard(lock_);
    //     auto alloc = virtual_to_device(virtual_inner_pool_base_, 1);
    //     return alloc.success ? alloc.device_addr : nullptr;
    // }
    
    // // Get allocation statistics
    // size_t get_leaf_nodes_allocated() const {
    //     SpinLockGuard guard(lock_);
    //     return leaf_offset_ / DPA_LEAF_NODE_SIZE;
    // }

    // size_t get_inner_nodes_allocated() const {
    //     SpinLockGuard guard(lock_);
    //     return inner_offset_ / DPA_INNER_NODE_SIZE;
    // }

    // Get pool usage in bytes
    size_t get_leaf_pool_usage() const {
        //SpinLockGuard guard(lock_);
        return leaf_offset_;
    }

    size_t get_inner_pool_usage() const {
        //SpinLockGuard guard(lock_);
        return inner_offset_;
    }

    // Reset allocator state
    void reset() {
        SpinLockGuard guard(lock_);
        leaf_offset_ = 0;
        inner_offset_ = 0;
        leaf_allocations_.clear();
        inner_allocations_.clear();
        free_leaf_offsets_.clear();
        free_inner_offsets_.clear();
    }
    
private:
    size_t range_size_;                         // Size of each DPA memory range
    std::vector<void*> dpa_ranges_;            // DPA memory ranges (all equal size)
    uintptr_t virtual_leaf_pool_base_;         // Virtual base of leaf pool
    uintptr_t virtual_inner_pool_base_;        // Virtual base of inner pool
    size_t leaf_offset_;                       // Current offset in leaf pool
    size_t inner_offset_;                      // Current offset in inner pool
    
    // Mapping from host addresses to DPA addresses
    std::unordered_map<void*, AllocationInfo> leaf_allocations_;
    std::unordered_map<void*, AllocationInfo> inner_allocations_;
    std::deque<uintptr_t> free_leaf_offsets_;
    std::deque<uintptr_t> free_inner_offsets_;
    mutable SpinLock lock_;
    
    // Convert virtual address to actual device address with boundary checking
    DeviceAllocation virtual_to_device(uintptr_t virtual_addr, size_t allocation_size) const {
        if (dpa_ranges_.empty()) {
            return {nullptr, virtual_addr, false};
        }
        
        size_t range_index = virtual_addr / range_size_;
        size_t offset_in_range = virtual_addr % range_size_;
        
        // Check if allocation crosses segment boundary
        if (offset_in_range + allocation_size > range_size_) {
            // Move to next segment, waste remaining space in current segment
            range_index++;
            offset_in_range = 0;
            
            // Recalculate virtual_addr to reflect the move to next segment
            virtual_addr = range_index * range_size_ + offset_in_range;
        }
        
        if (range_index >= dpa_ranges_.size()) {
            return {nullptr, virtual_addr, false}; // No more segments
        }
        
        void* device_addr = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(dpa_ranges_[range_index]) + offset_in_range
        );
        
        return {device_addr, virtual_addr, true};
    }
    
    // Align size to cache line boundary
    static size_t align_to_cache_line(size_t size) {
        return (size + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
    }
};

#endif // DPATREE_ALLOCATOR_HPP
