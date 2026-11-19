#ifndef HOST_LEARNED_TREE_HPP
#define HOST_LEARNED_TREE_HPP

#include "host_learned_tree_node.hpp"
#include "../pool_allocator.hpp"
#include "../shared/spin_lock.hpp"
#include "../shared/commands.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// PLA segment structure for building
struct PLASegment {
    uint64_t first_key;
    double slope;
    double intercept; // predicted global position when key == first_key
    size_t first_position;
    size_t last_position;
};

// PLA Builder class using PGM's optimal algorithm
class PLABuilder {
public:
    // Build PLA segments for KVPairs with epsilon bound and optional size constraint
    // Uses PGM's optimal piecewise linear approximation algorithm
    static std::vector<PLASegment> build_segments_for_kvpairs(
        const KVPair* data,
        size_t count,
        int epsilon,
        size_t max_size = SIZE_MAX  // Maximum keys per segment
    );

    // Build segments for children first keys (for inner nodes)
    static std::vector<PLASegment> build_segments_for_children(
        HostLearnedTreeNode** children,
        size_t count,
        int epsilon
    );
};

class HostLearnedTree {
private:
    volatile std::atomic<HostLearnedTreeNode*> root_;
    std::atomic<uint64_t> root_epoch_;
    
    // Pool allocators for nodes
    PoolAllocator* leaf_pool_;
    PoolAllocator* inner_pool_;
    
    // Statistics
    uint64_t num_leafs_;
    uint64_t num_inners_;
    uint64_t num_entries_;
    std::atomic<uint64_t> patches_processed_;
    std::atomic<uint64_t> structural_modifications_began;
    std::atomic<uint64_t> structural_modifications_enqueued;
    mutable SpinLock stats_lock_;
    

    struct TreeUpdateStats {
        std::atomic<uint64_t> first_update_ns;
        std::atomic<uint64_t> completion_update_ns;
        std::atomic<uint64_t> elements_processed;
        std::atomic<uint64_t> completion_elements;

        TreeUpdateStats()
            : first_update_ns(0)
            , completion_update_ns(0)
            , elements_processed(0)
            , completion_elements(0) {}
    };

    TreeUpdateStats update_stats_;
    
    // DPA integration
    void* host_global_area_;     // Host global area for stitch commands
    void* dpa_base_addr_;        // DPA base address for address pre-computation
    class DPATreeAllocator* dpa_allocator_;  // Shadow allocator for DPA addresses
    
public:
    mutable SpinLock root_lock_;

    struct PreparedCopyStitchCommand {
        uint32_t size;
        command_type_t type;
        alignas(command_stitch_copy_learned_inner_t) std::array<uint8_t, sizeof(command_stitch_copy_learned_inner_t)> buffer;
    };

    // Constructor takes pre-allocated buffers from host global area
    HostLearnedTree(void* leaf_buffer, size_t leaf_buffer_size,
                    void* inner_buffer, size_t inner_buffer_size,
                    void* host_global_area, 
                    const std::vector<void*>& dpa_segments, size_t segment_size);
    
    ~HostLearnedTree();
    
    // Core operations
    bool find(uint64_t key, uint64_t& value) const;
    
    // Tree building - main entry point
    void bulk_load(const std::vector<KVPair>& sorted_data);
    void log_partition_layout() const;
    
    // Process buffer patch from DPA
    void process_buffer_patch(HostLearnedLeafNode* leaf, 
                            const KVPair* buffer_data, size_t buffer_count);

    bool update_leaf(HostLearnedLeafNode* leaf,
                     const uint64_t* insert_buffer_keys_ptr,
                     const uint64_t* insert_buffer_values_ptr,
                     size_t insert_buffer_high,
                     size_t thread_id);

    HostLearnedInnerNode* get_and_lock_parent(HostLearnedTreeNode* node_ptr);
    
    // Statistics
    uint64_t get_num_entries() const {
        //SpinLockGuard guard(stats_lock_);
        return num_entries_;
    }
    uint64_t get_num_leafs() const {
        //SpinLockGuard guard(stats_lock_);
        return num_leafs_;
    }
    uint64_t get_num_inners() const {
        //SpinLockGuard guard(stats_lock_);
        return num_inners_;
    }
    size_t get_height() const;
    
    // Memory management - Host pools
    size_t get_leaf_pool_usage() const;
    size_t get_inner_pool_usage() const;

    // Memory management - DPA pools
    size_t get_dpa_leaf_pool_usage() const;
    size_t get_dpa_inner_pool_usage() const;
    
    // Get root for stitch generation
    HostLearnedTreeNode* get_root() const { return root_.load(std::memory_order_acquire); }
    
    // Get DPA allocator for address translation
    DPATreeAllocator* get_dpa_allocator() const { return dpa_allocator_; }
    
    // Send complete tree to DPA via stitch commands (for bulk load)
    void send_complete_tree_to_dpa();
    
private:
    // Helper functions
    HostLearnedLeafNode* allocate_leaf();
    HostLearnedInnerNode* allocate_inner();
    void deallocate_node(HostLearnedTreeNode* node);

    // Build helpers
    HostLearnedLeafNode* create_leaf_from_pgm_segment(const KVPair* data, size_t start, size_t end,
                                                      const PLASegment& segment, int epsilon);
    HostLearnedTreeNode* build_tree_recursive(std::vector<HostLearnedTreeNode*>& nodes);

    // PGM segmentation helpers
    std::vector<PLASegment> build_pgm_segments_for_keys(const uint64_t* keys, size_t count, int epsilon,
                                                        size_t max_segment_size = SIZE_MAX);
    std::vector<HostLearnedInnerNode*> create_inner_nodes_from_segments(
        const std::vector<PLASegment>& pgm_segments,
        const std::vector<HostLearnedTreeNode*>& nodes,
        const std::vector<uint64_t>& first_keys,
        uint16_t error_bound);

    // Materialise a sequence of inner nodes covering the provided children.
    // Every new node inherits the supplied `default_partition` unless
    // `inherit_partition_from_children` is set, in which case the children must
    // all agree on a partition and that partition is propagated instead.
    std::vector<HostLearnedInnerNode*> make_inner_nodes_for_children(
        const std::vector<HostLearnedTreeNode*>& children,
        uint16_t default_partition,
        bool inherit_partition_from_children = false);

    void assign_partitions_for_bulk_root(HostLearnedTreeNode* root);

    // Stitch helpers
    void emit_copy_stitches(const std::vector<HostLearnedTreeNode*>& nodes);
    void* ensure_dpa_mapping(HostLearnedTreeNode* node);
    void send_connect_stitch(HostLearnedInnerNode* parent, HostLearnedTreeNode* child, uint16_t child_index);
    void send_root_stitch(HostLearnedTreeNode* new_root);
    void send_unflag_stitch(HostLearnedLeafNode* leaf);
    void prepare_copy_stitch_commands(uint32_t queue_id,
                                      const std::vector<HostLearnedTreeNode*>& nodes,
                                      bool print_debug,
                                      std::vector<PreparedCopyStitchCommand>& out_commands);

    // Tree traversal
    HostLearnedLeafNode* find_leaf(uint64_t key) const;

    uint32_t queue_for_partition(uint16_t partition_id) const;
    uint32_t queue_for_leaf(const HostLearnedLeafNode* leaf) const;
    void emit_copy_stitches_on_queue(uint32_t queue_id,
                                     const std::vector<PreparedCopyStitchCommand>& commands,
                                     bool print_debug = false);
    void send_connect_stitch_on_queue(HostLearnedInnerNode* parent,
                                      HostLearnedTreeNode* child,
                                      uint16_t child_index,
                                      uint32_t queue_id);
    void send_root_stitch_on_queue(HostLearnedTreeNode* new_root,
                                   uint32_t queue_id);
    void send_unflag_stitch_on_queue(HostLearnedLeafNode* leaf, uint32_t queue_id);
    void broadcast_epoch_stitch(uint64_t epoch);

    void print_stats() const;
};

#endif // HOST_LEARNED_TREE_HPP
