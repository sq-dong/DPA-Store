#include <doca_log.h>
DOCA_LOG_REGISTER(HOST_LEARNED_TREE_NODE)

#include "host_learned_tree_node.hpp"
#include <algorithm>
#include <cmath>
#include <limits>


std::atomic<uint64_t> HostLearnedTreeNode::global_uuid_version_counter_{0};

uint64_t HostLearnedTreeNode::next_uuid_version() {
    return global_uuid_version_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
}


// HostLearnedLeafNode implementation

HostLearnedLeafNode::HostLearnedLeafNode()
    : HostLearnedTreeNode(LEARNED_LEAF)
    , slope_fp(0)
    , intercept_fp(0)
    , sorted_count(0)
    , buffer_count(0)
    , first_key(0) {
    std::memset(sorted_keys, 0, sizeof(sorted_keys));
    std::memset(sorted_values, 0, sizeof(sorted_values));
    std::memset(insert_buffer_keys, 0, sizeof(insert_buffer_keys));
    std::memset(insert_buffer_values, 0, sizeof(insert_buffer_values));
}

void HostLearnedLeafNode::set_model(double slope, double intercept) {
    slope_fp = to_fixed_point(slope);
    intercept_fp = static_cast<int64_t>(std::llround(intercept));
}

void HostLearnedLeafNode::set_sorted_data(const KVPair* data, size_t count) {
    sorted_count = std::min(count, static_cast<size_t>(LEARNED_KEYS_PER_LEAF));
    // Copy keys and values separately
    for (size_t i = 0; i < sorted_count; i++) {
        sorted_keys[i] = data[i].key;
        sorted_values[i] = data[i].value;
    }

    if (sorted_count > 0) {
        first_key = sorted_keys[0];
    }
}

bool HostLearnedLeafNode::find(uint64_t key, uint64_t& value) const {
    // First check insert buffer (recent inserts)
    for (uint16_t i = 0; i < buffer_count; i++) {
        if (insert_buffer_keys[i] == key) {
            value = insert_buffer_values[i];
            return true;
        }
    }
    
    // Then use model for sorted area
    if (sorted_count == 0 || key < first_key) {
        return false;
    }
    
    // Apply model to get predicted position
    uint64_t delta = key - first_key;  // guaranteed non-negative by earlier guard
    __uint128_t normalized_key = (__uint128_t)delta;
    __uint128_t product = normalized_key * (__uint128_t)slope_fp;
    __uint128_t base128 = product >> FIXED_POINT_SHIFT;
    int64_t base64 = (int64_t)base128;
    int64_t sum = base64 + intercept_fp;
    int predicted_pos = (int)sum;

    // Clamp to valid range
    predicted_pos = std::max(0, std::min(static_cast<int>(sorted_count - 1), predicted_pos));

    // Binary search within epsilon range
    // Use TRAVERSE epsilon for searching, not BUILD epsilon
    const int traverse_epsilon = EPSILON_TRAVERSE_LEAF;
    int start = std::max(0, predicted_pos - traverse_epsilon);
    int end = std::min(static_cast<int>(sorted_count), predicted_pos + traverse_epsilon + 1);

    for (int i = start; i < end; i++) {
        if (sorted_keys[i] == key) {
            value = sorted_values[i];
            return true;
        }
        if (sorted_keys[i] > key) {
            break;
        }
    }

    DOCA_LOG_INFO("  Key not found after searching range");
    
    return false;
}

// HostLearnedInnerNode implementation

HostLearnedInnerNode::HostLearnedInnerNode()
    : HostLearnedTreeNode(LEARNED_INNER)
    , num_segments(0)
    , num_children(0)
    , min_key(0)
    , stale(false) {
    std::memset(segments, 0, sizeof(segments));
    std::memset(children, 0, sizeof(children));
}

HostLearnedInnerNode::HostLearnedInnerNode(const std::vector<LearnedSegment>& input_segments,
                                           const std::vector<HostLearnedTreeNode*>& input_children,
                                           uint16_t error_bound)
    : HostLearnedTreeNode(LEARNED_INNER)
    , num_segments(0)
    , num_children(0)
    , min_key(UINT64_MAX)
    , stale(false)
    , result_from_root_split(0) {

    // Initialize arrays
    std::memset(segments, 0, sizeof(segments));
    std::memset(children, 0, sizeof(children));

    // Copy segments (up to max capacity)
    num_segments = std::min(input_segments.size(),
                           static_cast<size_t>(LEARNED_SEGMENTS_PER_INNER));
    for (size_t i = 0; i < num_segments; i++) {
        segments[i] = input_segments[i];
    }

    // Copy children (up to max capacity)
    num_children = std::min(input_children.size(),
                           static_cast<size_t>(LEARNED_SEGMENTS_PER_INNER * LEARNED_KEYS_PER_INNER_SEGMENT));
    for (size_t i = 0; i < num_children; i++) {
        children[i] = input_children[i];

        // Set parent pointer
        if (children[i]) {
            // Child nodes remain parent-less for traversal; we find parents on demand.
        }
    }

    // Compute key range from children
    compute_key_range();
}

void HostLearnedInnerNode::compute_key_range() {
    min_key = UINT64_MAX;

    for (size_t i = 0; i < num_children; i++) {
        if (!children[i]) continue;

        if (children[i]->type == LEARNED_LEAF) {
            auto* leaf = static_cast<HostLearnedLeafNode*>(children[i]);
            min_key = std::min(min_key, leaf->first_key);
        } else {
            auto* inner = static_cast<HostLearnedInnerNode*>(children[i]);
            min_key = std::min(min_key, inner->min_key);
        }
    }

    // Handle empty case
    if (min_key == UINT64_MAX) {
        min_key = 0;
    }
}

int HostLearnedInnerNode::find_segment(uint64_t key) const {
    // Binary search for segment covering key
    int left = 0;
    int right = num_segments - 1;
    
    while (left < right) {
        int mid = (left + right + 1) / 2;
        if (segments[mid].first_key <= key) {
            left = mid;
        } else {
            right = mid - 1;
        }
    }
    
    return left;
}

HostLearnedTreeNode* HostLearnedInnerNode::find_child(uint64_t key) const {
    if (num_segments == 0 || num_children == 0) {
        return nullptr;
    }

    // Find segment covering key
    int seg_idx = find_segment(key);
    const LearnedSegment& seg = segments[seg_idx];


    // For segments with few children, just do a linear scan
    // The model doesn't work well when mapping huge key ranges to 0-2 indices
    if (false && seg.num_children <= 4) {
        // Linear scan through segment's children
        for (uint16_t i = 0; i < seg.num_children; i++) {
            uint16_t child_idx = seg.first_child_idx + i;
            if (child_idx >= num_children || !children[child_idx]) continue;
            
            // Check if this is the right child by comparing with next child's first key
            if (i + 1 < seg.num_children) {
                uint16_t next_idx = seg.first_child_idx + i + 1;
                if (next_idx < num_children && children[next_idx]) {
                    uint64_t next_first_key = 0;
                    if (children[next_idx]->type == LEARNED_LEAF) {
                        next_first_key = static_cast<HostLearnedLeafNode*>(children[next_idx])->first_key;
                    } else {
                        next_first_key = static_cast<HostLearnedInnerNode*>(children[next_idx])->min_key;
                    }
                    if (key < next_first_key) {
                        return children[child_idx];
                    }
                }
            } else {
                // Last child in segment
                return children[child_idx];
            }
        }
        // Shouldn't reach here, but return first child as fallback
        return children[seg.first_child_idx];
    }
    
    // For larger segments, use the model (though we don't have any in this case)

    uint64_t delta = key - seg.first_key;
    __uint128_t normalized_key = (__uint128_t)delta;
    __uint128_t product = normalized_key * (__uint128_t)seg.slope_fp;
    __uint128_t base128 = product >> FIXED_POINT_SHIFT;
    int64_t base64 = (int64_t)base128;
    int64_t sum = base64 + seg.intercept_fp;
    int predicted_offset = (int)sum;

    // Clamp to segment's child range
    predicted_offset = std::max(0, std::min(static_cast<int>(seg.num_children - 1), predicted_offset));
    
    // Calculate absolute child index
    int predicted_child = seg.first_child_idx + predicted_offset;

    // Binary search within epsilon range
    // Use TRAVERSE epsilon for searching, not BUILD epsilon
    const int traverse_epsilon = EPSILON_TRAVERSE_INNER;
    int start = std::max(static_cast<int>(seg.first_child_idx), predicted_child - traverse_epsilon);
    int end = std::min(static_cast<int>(seg.first_child_idx + seg.num_children),
                       predicted_child + traverse_epsilon + 1);

    
    // Find actual child by checking first keys
    for (int i = start; i < end && i < num_children; i++) {
        if (!children[i]) continue;
        
        uint64_t child_first_key = 0;
        if (children[i]->type == LEARNED_LEAF) {
            child_first_key = static_cast<HostLearnedLeafNode*>(children[i])->first_key;
        } else {
            child_first_key = static_cast<HostLearnedInnerNode*>(children[i])->min_key;
        }
        
        // Check if this child or next contains the key
        if (i + 1 < num_children && children[i + 1]) {
            uint64_t next_first_key = 0;
            if (children[i + 1]->type == LEARNED_LEAF) {
                next_first_key = static_cast<HostLearnedLeafNode*>(children[i + 1])->first_key;
            } else {
                next_first_key = static_cast<HostLearnedInnerNode*>(children[i + 1])->min_key;
            }
            
            if (key < next_first_key) {
                return children[i];
            }
        } else {
            // Last child in range
            return children[i];
        }
    }
    
    // Fallback to predicted child
    if (predicted_child < num_children) {
        return children[predicted_child];
    }

    return nullptr;
}

// Note: update_segments removed - segments are now immutable after construction
// Any changes require creating a new inner node with new segments via the constructor
