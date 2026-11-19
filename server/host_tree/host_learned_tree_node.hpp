#ifndef HOST_LEARNED_TREE_NODE_HPP
#define HOST_LEARNED_TREE_NODE_HPP

#include "../shared/tree_config.h"
#include "../shared/server_config.h"
#include "../shared/spin_lock.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

// Reader-writer lock guarding structural updates on inner nodes. We keep it
// header-only so patchers can acquire/release while traversing the tree.
using NodeSpinLock = SpinLock;

// Forward declare or define KVPair if not already defined
#ifndef KVPAIR_DEFINED
#define KVPAIR_DEFINED
struct KVPair {
    uint64_t key;
    uint64_t value;
    
    KVPair() : key(0), value(0) {}
    KVPair(uint64_t k, uint64_t v) : key(k), value(v) {}
};
#endif

// Forward declaration for fixed-point helpers
inline uint64_t to_fixed_point(double value);

// Learned segment structure for inner nodes
struct LearnedSegment {
    uint64_t first_key;         // First key this segment covers
    uint64_t slope_fp;          // Fixed-point slope (64-bit for higher precision)
    int64_t intercept_fp;       // Integer intercept (added after shifting)
    uint16_t first_child_idx;   // Index of first child in children array
    uint16_t num_children;      // Number of children covered by this segment
    uint32_t padding;           // Alignment padding

    // Constructor with direct initialization
    LearnedSegment() : first_key(0), slope_fp(0), intercept_fp(0),
                       first_child_idx(0), num_children(0), padding(0) {}

    LearnedSegment(uint64_t key, double slope, double intercept,
                   uint16_t first_idx, uint16_t child_count)
        : first_key(key),
          slope_fp(to_fixed_point(slope)),
          intercept_fp(static_cast<int64_t>(std::llround(intercept))),
          first_child_idx(first_idx),
          num_children(child_count),
          padding(0) {}
};

// Base learned node class
class HostLearnedTreeNode  {
public:
    enum NodeType : uint8_t {
        LEARNED_LEAF = 0,
        LEARNED_INNER = 1
    };
    
    NodeType type;
    uint8_t reserved0;
    uint16_t partition_id;
    uint32_t reserved1;
    uint64_t UUID_version;
    
    HostLearnedTreeNode(NodeType t)
        : type(t)
        , reserved0(0)
        , partition_id(0)
        , reserved1(0)
        , UUID_version(next_uuid_version()) {}
    virtual ~HostLearnedTreeNode() = default;

protected:
    static uint64_t next_uuid_version();

private:
    static std::atomic<uint64_t> global_uuid_version_counter_;
};

// Learned leaf node with insert buffer
class HostLearnedLeafNode : public HostLearnedTreeNode {
public:
    // Model parameters (64-bit for higher precision)
    uint64_t slope_fp;                              // Fixed-point slope
    int64_t intercept_fp;                           // Integer intercept (added post-shift)

    char padding[32];

    // Key-value separated sorted area for cache-efficient scanning
    uint64_t sorted_keys[LEARNED_KEYS_PER_LEAF];      // Sorted keys
    uint64_t sorted_values[LEARNED_KEYS_PER_LEAF];    // Sorted values

    // Key-value separated insert buffer
    uint64_t insert_buffer_keys[LEARNED_INSERT_BUFFER_SIZE];    // Buffer keys (16 entries)
    uint64_t insert_buffer_values[LEARNED_INSERT_BUFFER_SIZE];  // Buffer values
    
    uint16_t sorted_count;                          // Number of sorted entries
    uint16_t buffer_count;                           // Number of buffer entries

    uint32_t padding2;

    
    // Key range
    uint64_t first_key;                             // First key in sorted area
    uint64_t last_key_DEPRECATED;                              // Last key in sorted area

    uint64_t padding3[4];
    
    HostLearnedLeafNode();
    
    // Set model from floating-point values
    void set_model(double slope, double intercept);
    
    // Set sorted data
    void set_sorted_data(const KVPair* data, size_t count);
    
    // Find key using model + bounded search
    bool find(uint64_t key, uint64_t& value) const;
    
};

// Learned inner node with segments
class HostLearnedInnerNode : public HostLearnedTreeNode {
public:
    // Segments for routing
    LearnedSegment segments[LEARNED_SEGMENTS_PER_INNER];  // Segments array
    uint16_t num_segments;                                // Actual segments used

    // Child pointers
    // Maximum children = LEARNED_SEGMENTS_PER_INNER * LEARNED_KEYS_PER_INNER_SEGMENT
    HostLearnedTreeNode* children[LEARNED_SEGMENTS_PER_INNER * LEARNED_KEYS_PER_INNER_SEGMENT];
    uint16_t num_children;                                     // Actual children

    // Key range
    uint64_t min_key;                                     // Minimum key in subtree
    uint64_t max_key_DEPRECATED;                                     // Maximum key in subtree

    NodeSpinLock node_lock;                               // Lock for structural updates during patches
    std::atomic<bool> stale;                              // Marks node as decoupled from the active tree

    uint64_t result_from_root_split; 

    uint64_t padding3[3];                                // Tail padding to maintain cache-line multiple

    // Default constructor
    HostLearnedInnerNode();

    // Constructor with segments and children
    HostLearnedInnerNode(const std::vector<LearnedSegment>& input_segments,
                        const std::vector<HostLearnedTreeNode*>& input_children,
                        uint16_t error_bound = EPSILON_BUILD_RUNTIME_INNER);

    // Find child for given key using segments
    HostLearnedTreeNode* find_child(uint64_t key) const;

    // Get segment covering key
    int find_segment(uint64_t key) const;

    // Check if full
    bool is_full() const {
        return num_children >= (LEARNED_SEGMENTS_PER_INNER * LEARNED_KEYS_PER_INNER_SEGMENT);
    }

    // Initialize key range from children
    void compute_key_range();
};

// Helper functions for fixed-point conversion
inline uint64_t to_fixed_point(double value) {
    const __uint128_t scale = static_cast<__uint128_t>(1) << FIXED_POINT_SHIFT;
    long double scaled = static_cast<long double>(value) * static_cast<long double>(scale);
    if (scaled <= 0.0L) {
        return 0;
    }
    __uint128_t fp = static_cast<__uint128_t>(scaled);
    const __uint128_t max = std::numeric_limits<uint64_t>::max();
    if (fp > max) {
        return static_cast<uint64_t>(max);
    }
    return static_cast<uint64_t>(fp);
}


static_assert(sizeof(HostLearnedLeafNode) % CACHE_LINE_SIZE == 0,
              "HostLearnedLeafNode size must align to cache line");
static_assert(sizeof(HostLearnedInnerNode) % CACHE_LINE_SIZE == 0,
              "HostLearnedInnerNode size must align to cache line");

#endif // HOST_LEARNED_TREE_NODE_HPP
