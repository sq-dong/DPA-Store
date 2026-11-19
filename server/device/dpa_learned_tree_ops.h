#ifndef DPA_LEARNED_TREE_OPS_H
#define DPA_LEARNED_TREE_OPS_H

#include "dpa_learned_tree_types.h"
#include "spinlock.h"
#include <stddef.h>
#include "../shared/host_global_area.h"
#include "../shared/commands.h"
#include "../shared/server_config.h"
#include "../../common/dpastore_common.h"

#ifndef DPASTORE_DEBUG_RANGE
#define DPASTORE_DEBUG_RANGE 0
#endif

/* Tree operation functions for DPA learned tree */
/* Define USE_BTREE_ALGORITHM to bypass learned predictions and use binary search traversal */
//#define USE_BTREE_ALGORITHM
/* Node type checking functions */
static inline bool dpa_learned_node_is_leaf(const DPALearnedTreeNode* node) {
    return node->base.type == DPA_LEARNED_LEAF;
}

static inline bool dpa_learned_node_is_inner(const DPALearnedTreeNode* node) {
    return node->base.type == DPA_LEARNED_INNER;
}

/* Find segment in inner node covering the given key */
static inline int dpa_learned_inner_find_segment(const DPALearnedInnerNode* inner, uint64_t key) {
    /* Unrolled binary search for max 7 segments - unused slots filled with UINT64_MAX */
    /* Reduces comparisons from 6 to 3 in worst case */
    const uint64_t* first_keys = inner->segment_first_keys;

    /* Binary search tree unrolled:
     * Check middle (index 3), then left/right half, then final position
     * Tree structure for indices 0-6:
     *           3
     *       /       \
     *      1         5
     *    /   \     /   \
     *   0     2   4     6
     */

    if (key < first_keys[3]) {
        /* Left half: segments 0-2 */
        if (key < first_keys[1]) {
            return 0;  /* Segment 0 */
        } else if (key < first_keys[2]) {
            return 1;  /* Segment 1 */
        } else {
            return 2;  /* Segment 2 */
        }
    } else {
        /* Right half: segments 3-6 */
        if (key < first_keys[5]) {
            if (key < first_keys[4]) {
                return 3;  /* Segment 3 */
            } else {
                return 4;  /* Segment 4 */
            }
        } else if (key < first_keys[6]) {
            return 5;  /* Segment 5 */
        } else {
            return 6;  /* Segment 6 */
        }
    }
}

/* Find child in inner node for given key using learned model */
static inline DPALearnedTreeNode* dpa_learned_inner_find_child(const DPALearnedInnerNode* inner, uint64_t key) {
#ifdef USE_BTREE_ALGORITHM
    if (inner->num_children == 0) {
        return NULL;
    }

    int left = 0;
    int right = inner->num_children - 1;
    while (left < right) {
        int mid = left + ((right - left + 1) >> 1);
        if (key < inner->child_first_keys[mid]) {
            right = mid - 1;
        } else {
            left = mid;
        }
    }

    return (DPALearnedTreeNode*)inner->children[left];
#else
    
    // /* Check key bounds */
    // if (key < inner->min_key) {
    //     return (DPALearnedTreeNode*)inner->children[0];
    // }
    // if (key > inner->max_key) {
    //     return (DPALearnedTreeNode*)inner->children[inner->num_children - 1];
    // }
    
    /* Find segment covering this key */
    int seg_idx = dpa_learned_inner_find_segment(inner, key);
    // if (seg_idx < 0) return NULL;
    
    const DPALearnedSegment* seg = &inner->segments[seg_idx];
    
    /* For small segments, use linear scan */
    if (__builtin_expect(seg->num_children <= 4, 0)) {
        /* Linear scan through segment's children - check child_first_keys array */
        for (uint16_t i = 0; i < seg->num_children; i++) {
            uint16_t child_idx = seg->first_child_idx + i;
            if (child_idx >= inner->num_children) break;

            /* Check if this is the right child by comparing with next child's first key */
            if (i < seg->num_children - 1) {
                uint16_t next_idx = seg->first_child_idx + i + 1;
                if (key < inner->child_first_keys[next_idx]) {
                    return (DPALearnedTreeNode*)inner->children[child_idx];
                }
            } else {
                /* Last child in segment - return it */
                return (DPALearnedTreeNode*)inner->children[child_idx];
            }
        }

        /* Default to first child of segment if no exact match */
        return (DPALearnedTreeNode*)inner->children[seg->first_child_idx];
    }
    
    /* Use model prediction for larger segments */
    /* Apply model to get predicted position */
    /* Get first_key from the compact array */
    uint64_t segment_first_key = inner->segment_first_keys[seg_idx];

    uint64_t delta = key - segment_first_key;  // guaranteed non-negative by earlier guard
    __uint128_t normalized_key = (__uint128_t)delta;
    __uint128_t product = normalized_key * (__uint128_t)seg->slope_fp;
    __uint128_t base128 = product >> FIXED_POINT_SHIFT;
    int64_t base64 = (int64_t)base128;
    int64_t sum = base64 + seg->intercept_fp;
    int predicted_offset = (int)sum;

    /* Clamp to valid range */
    predicted_offset = (predicted_offset < 0) ? 0 : predicted_offset;
    predicted_offset = (predicted_offset >= seg->num_children) ? seg->num_children - 1 : predicted_offset;

    /* Calculate absolute child index */
    int predicted_child = seg->first_child_idx + predicted_offset;

    // /* Prefetch the predicted child pointer and its neighbors */
    __builtin_prefetch(&inner->children[predicted_child], 0, 0);

    /* Binary search within epsilon range - use TRAVERSE epsilon */
    int traverse_epsilon = EPSILON_TRAVERSE_INNER;
    int search_start = (predicted_child >= traverse_epsilon) ?
                       predicted_child - traverse_epsilon :
                       seg->first_child_idx;
    int search_end = predicted_child + traverse_epsilon + 1;
    if (__builtin_expect(search_end > seg->first_child_idx + seg->num_children, 0)) {
        search_end = seg->first_child_idx + seg->num_children;
    }
    
    /* Bounded search within epsilon range - use child_first_keys array for efficient comparison */
    for (int i = search_start; i < search_end && i < inner->num_children; i++) {
        /* Check if this is the right child by comparing with next child's first key */
        if (__builtin_expect(i < inner->num_children - 1, 1)) {
            if (key < inner->child_first_keys[i + 1]) {
                return (DPALearnedTreeNode*)inner->children[i];
            }
        } else {
            /* Last child - return it */
            return (DPALearnedTreeNode*)inner->children[i];
        }
    }

    /* Return predicted child if no exact match found */
    return (DPALearnedTreeNode*)inner->children[predicted_child];
#endif
}

static inline void dpa_learned_range_track_pivot(const DPALearnedInnerNode* inner,
                                                 uint64_t key,
                                                 uint16_t child_idx,
                                                 uint64_t* next_key,
                                                 bool* next_key_valid) {
    if (!next_key || !next_key_valid) {
        return;
    }
    if (child_idx + 1 >= inner->num_children) {
        return;
    }
    uint64_t candidate = inner->child_first_keys[child_idx + 1];
    if (candidate <= key) {
        return;
    }
    if (!(*next_key_valid) || candidate < *next_key) {
        *next_key = candidate;
        *next_key_valid = true;
    }
}

static inline DPALearnedTreeNode* dpa_learned_inner_find_child_range(const DPALearnedInnerNode* inner,
                                                                     uint64_t key,
                                                                     uint64_t* next_key,
                                                                     bool* next_key_valid) {
#ifdef USE_BTREE_ALGORITHM
    if (inner->num_children == 0) {
        return NULL;
    }

    int left = 0;
    int right = inner->num_children - 1;
    while (left < right) {
        int mid = left + ((right - left + 1) >> 1);
        if (key < inner->child_first_keys[mid]) {
            right = mid - 1;
        } else {
            left = mid;
        }
    }

    uint16_t child_idx = (uint16_t)left;
    dpa_learned_range_track_pivot(inner, key, child_idx, next_key, next_key_valid);
    return (DPALearnedTreeNode*)inner->children[child_idx];
#else
    int seg_idx = dpa_learned_inner_find_segment(inner, key);
    const DPALearnedSegment* seg = &inner->segments[seg_idx];

    if (__builtin_expect(seg->num_children <= 4, 0)) {
        for (uint16_t i = 0; i < seg->num_children; i++) {
            uint16_t child_idx = seg->first_child_idx + i;
            if (child_idx >= inner->num_children) break;

            if (i < seg->num_children - 1) {
                uint16_t next_idx = seg->first_child_idx + i + 1;
                if (key < inner->child_first_keys[next_idx]) {
                    dpa_learned_range_track_pivot(inner, key, child_idx, next_key, next_key_valid);
                    return (DPALearnedTreeNode*)inner->children[child_idx];
                }
            } else {
                dpa_learned_range_track_pivot(inner, key, child_idx, next_key, next_key_valid);
                return (DPALearnedTreeNode*)inner->children[child_idx];
            }
        }

        uint16_t fallback = seg->first_child_idx;
        dpa_learned_range_track_pivot(inner, key, fallback, next_key, next_key_valid);
        return (DPALearnedTreeNode*)inner->children[fallback];
    }

    uint64_t segment_first_key = inner->segment_first_keys[seg_idx];

    uint64_t delta = key - segment_first_key;  // guaranteed non-negative by earlier guard
    __uint128_t normalized_key = (__uint128_t)delta;
    __uint128_t product = normalized_key * (__uint128_t)seg->slope_fp;
    __uint128_t base128 = product >> FIXED_POINT_SHIFT;
    int64_t base64 = (int64_t)base128;
    int64_t sum = base64 + seg->intercept_fp;
    int predicted_offset = (int)sum;

    predicted_offset = (predicted_offset < 0) ? 0 : predicted_offset;
    predicted_offset = (predicted_offset >= seg->num_children) ? seg->num_children - 1 : predicted_offset;

    int predicted_child = seg->first_child_idx + predicted_offset;

    __builtin_prefetch(&inner->children[predicted_child], 0, 0);

    int traverse_epsilon = EPSILON_TRAVERSE_INNER;
    int search_start = (predicted_child >= traverse_epsilon) ?
                       predicted_child - traverse_epsilon :
                       seg->first_child_idx;
    int search_end = predicted_child + traverse_epsilon + 1;
    if (__builtin_expect(search_end > seg->first_child_idx + seg->num_children, 0)) {
        search_end = seg->first_child_idx + seg->num_children;
    }

    for (int i = search_start; i < search_end && i < inner->num_children; i++) {
        if (__builtin_expect(i < inner->num_children - 1, 1)) {
            if (key < inner->child_first_keys[i + 1]) {
                dpa_learned_range_track_pivot(inner, key, (uint16_t)i, next_key, next_key_valid);
                return (DPALearnedTreeNode*)inner->children[i];
            }
        } else {
            dpa_learned_range_track_pivot(inner, key, (uint16_t)i, next_key, next_key_valid);
            return (DPALearnedTreeNode*)inner->children[i];
        }
    }

    dpa_learned_range_track_pivot(inner, key, (uint16_t)predicted_child, next_key, next_key_valid);
    return (DPALearnedTreeNode*)inner->children[predicted_child];
#endif
}

/* Find key in leaf node using learned model */
static inline bool dpa_learned_leaf_find(const DPALearnedLeafNode* leaf, uint64_t key, uint64_t* value,
                                         host_global_area_t* hga) {

#define LEAF_INSERTBUFFER
//#define LEAF_IB_DMA

#define LEAF_V_DMA

#ifdef LEAF_INSERTBUFFER
#ifdef LEAF_IB_DMA
    const uint64_t* insert_buffer_keys = (const uint64_t*)(((uintptr_t) hga) + leaf->host_hga_to_sorted_keys_offset + LEARNED_KEYS_PER_LEAF*16);
#else
    const uint64_t* insert_buffer_keys = leaf->buffer_keys;
#endif
    if (leaf->buffer_high > 0) {
        //__builtin_prefetch(insert_buffer_keys, 0, 0);
        if (leaf->buffer_high > 8) {
            __builtin_prefetch(insert_buffer_keys+8, 0, 0);
        }
    }
#endif

#ifdef LEAF_V_DMA
    const uint64_t* sorted_keys = (const uint64_t*)(((uintptr_t) hga) + leaf->host_hga_to_sorted_keys_offset);
    const uint64_t* sorted_values = (const uint64_t*)(((uintptr_t) hga) + leaf->host_hga_to_sorted_keys_offset + LEARNED_KEYS_PER_LEAF*8);
#else
#ifdef USE_MICRO_LEAVES
#error "USE_MICRO_LEAVES requires LEAF_V_DMA for sorted key/value access"
#endif
    const uint64_t* sorted_keys = leaf->sorted_keys;
    const uint64_t* sorted_values = leaf->sorted_values;
#endif

// const uint64_t* sorted_keys;
// const uint64_t* sorted_values;
// if (key % 1000 <= 900) {
//     sorted_keys = (const uint64_t*)(((uintptr_t) hga) + leaf->host_hga_to_sorted_keys_offset);
//     sorted_values = (const uint64_t*)(((uintptr_t) hga) + leaf->host_hga_to_sorted_keys_offset + LEARNED_KEYS_PER_LEAF*8);
// } else {
//     sorted_keys = leaf->sorted_keys;
//     sorted_values = leaf->sorted_values;
// }

#ifdef USE_BTREE_ALGORITHM

    /* First check insert buffer - key-value separated for fast scan */
    /* Keys are contiguous in memory for 1-2 cache line access */
    for (uint16_t i = 0; i < leaf->buffer_high; i++) {
        if (insert_buffer_keys[i] == key) {
            if (value) {
                #ifdef LEAF_IB_DMA
                const uint64_t* insert_buffer_values = (const uint64_t*)(((uintptr_t) hga) + leaf->host_hga_to_sorted_keys_offset + LEARNED_KEYS_PER_LEAF*16 + 16*8);
                #else
                const uint64_t* insert_buffer_values = leaf->buffer_values;
                #endif
                *value = insert_buffer_values[i];
                //LOG_I("Key %lu found in insert buffer with value %lu\n", key, insert_buffer_values[i]);
            }
            return true;
        }
    }

    if (leaf->sorted_count == 0) {
        return false;
    }

    int left = 0;
    int right = leaf->sorted_count - 1;
    while (left <= right) {
        int mid = left + ((right - left) >> 1);
        uint64_t mid_key = sorted_keys[mid];
        if (mid_key == key) {
            if (value) {
                *value = sorted_values[mid];
            }
            return true;
        }
        if (key < mid_key) {
            right = mid - 1;
        } else {
            left = mid + 1;
        }
    }

    return false;
#else

    /* Use model to predict position */
    uint64_t delta = key - leaf->first_key;  // guaranteed non-negative by earlier guard
    __uint128_t normalized_key = (__uint128_t)delta;
    __uint128_t product = normalized_key * (__uint128_t)leaf->slope_fp;
    __uint128_t base128 = product >> FIXED_POINT_SHIFT;
    int64_t base64 = (int64_t)base128;
    int64_t sum = base64 + leaf->intercept_fp;
    int predicted = (int)sum;
    
#ifdef BRANCHLESS
    /* Branchless clamping to [0, sorted_count-1] using bit operations */
    int sign_mask = predicted >> 31;  // All 1s if negative, 0 otherwise
    predicted = predicted & ~sign_mask;  // Set to 0 if negative

    /* Branchless min with sorted_count - 1 */
    int max_idx = leaf->sorted_count - 1;
    int diff = predicted - max_idx;
    int over_mask = diff >> 31;  // All 1s if predicted < max_idx, 0 otherwise
    predicted = (predicted & over_mask) | (max_idx & ~over_mask);
#else
    /* Traditional branched clamping */
    if (__builtin_expect(predicted < 0, 0)) predicted = 0;
    if (__builtin_expect(predicted >= leaf->sorted_count, 0)) predicted = leaf->sorted_count - 1;
#endif

    /* Prefetch predicted location */
    //__builtin_prefetch(sorted_keys + predicted, 0, 0);

    /* Binary search within epsilon range - use TRAVERSE epsilon */
    int traverse_epsilon = EPSILON_TRAVERSE_LEAF;

#ifdef BRANCHLESS
    /* Branchless calculation of search bounds */
    /* Branchless search_start: max(0, predicted - epsilon) */
    int start_raw = predicted - traverse_epsilon;
    int start_sign = start_raw >> 31;  // All 1s if negative
    int search_start = start_raw & ~start_sign;  // Set to 0 if negative

    /* Branchless search_end: min(sorted_count, predicted + epsilon + 1) */
    int search_end = predicted + traverse_epsilon + 1;
    int sorted_count = leaf->sorted_count;
    int end_diff = search_end - sorted_count;
    int end_sign = end_diff >> 31;  // All 1s if search_end < sorted_count
    search_end = (search_end & end_sign) | (sorted_count & ~end_sign);
#else
    /* Traditional branched bounds calculation */
    int search_start = 0; 
    if (__builtin_expect(predicted >= traverse_epsilon, 1)) search_start = predicted - traverse_epsilon;
    int search_end = predicted + traverse_epsilon + 1;
    if (__builtin_expect(search_end > leaf->sorted_count, 0)) search_end = leaf->sorted_count;
#endif


#ifdef LEAF_INSERTBUFFER

    /* First check insert buffer - key-value separated for fast scan */
    /* Keys are contiguous in memory for 1-2 cache line access */
    for (uint16_t i = 0; i < leaf->buffer_high; i++) {
        if (insert_buffer_keys[i] == key) {
            if (value) {
                #ifdef LEAF_IB_DMA
                const uint64_t* insert_buffer_values = (const uint64_t*)(((uintptr_t) hga) + leaf->host_hga_to_sorted_keys_offset + LEARNED_KEYS_PER_LEAF*16 + 16*8);
                #else
                const uint64_t* insert_buffer_values = leaf->buffer_values;
                #endif
                *value = insert_buffer_values[i];
                //LOG_I("Key %lu found in insert buffer with value %lu\n", key, insert_buffer_values[i]);
            }
            return true;
        }
    }

#endif

    // //Additional accesses for testing (random)
    // for (int i = 0; i < 8; i++) {
    //     int shift = key % 50;
    //     if (leaf->sorted_values[200+(i+shift)*(i+shift)] == 99)
    //         return true;
    // }

    // //Additional accesses for testing (sequential)
    // for (int i = 0; i < 8; i++) {
    //     int shift = key % 300;
    //     if (leaf->sorted_values[200+shift+(i*8)] == 99)
    //         return true;
    // }

    // //Additional compute for testing
    // size_t a;
    // for (size_t i = 0; i < 16; i++) {
    //     a += key^((size_t) i);
    // }
    // if (a == 4242)
    //     return false;

    // // //Additional compute for testing (old)
    // size_t a;
    // for (size_t i = 0; i < 64; i++) {
    //     a += i^((size_t) 99);
    // }
    // if (a == 4242)
    //     return true;

    /* Linear search within epsilon bounds */
    for (int i = search_start; i < search_end; i++) {
        if (sorted_keys[i] == key) {
            if (value) {
                *value = sorted_values[i];
            }
            //LOG_I("Key %lu found at index %d with value %lu\n", key, i, sorted_values[i]);
            return true;
        }
        if (sorted_keys[i] > key) {
            break;
        }
    }

    // LOG_I("Key %lu not found in leaf (first_key=%lu)\n", key, leaf->first_key);
    return false;
#endif
}

static inline void dpa_range_sort_entries(DPAStoreRangeEntry* entries, uint16_t count) {
    for (uint16_t i = 1; i < count; ++i) {
        DPAStoreRangeEntry tmp = entries[i];
        int j = (int)i - 1;
        while (j >= 0 && entries[j].key > tmp.key) {
            entries[j + 1] = entries[j];
            --j;
        }
        entries[j + 1] = tmp;
    }
}

static inline uint16_t dpa_learned_leaf_find_range(const DPALearnedLeafNode* leaf,
                                                   uint64_t start_key,
                                                   DPAStoreRangeEntry* out_entries,
                                                   uint16_t max_entries,
                                                   bool* leaf_exhausted,
                                                   host_global_area_t* hga) {
    if (leaf_exhausted) {
        *leaf_exhausted = true;
    }
    if (__builtin_expect(max_entries == 0, 0)) {
        return 0;
    }

#ifdef LEAF_INSERTBUFFER
#ifdef LEAF_IB_DMA
    const uint64_t* insert_buffer_keys = (const uint64_t*)(((uintptr_t) hga) +
        leaf->host_hga_to_sorted_keys_offset + LEARNED_KEYS_PER_LEAF * 16);
    const uint64_t* insert_buffer_values = (const uint64_t*)(((uintptr_t) hga) +
        leaf->host_hga_to_sorted_keys_offset + LEARNED_KEYS_PER_LEAF * 16 + 16 * 8);
#else
    const uint64_t* insert_buffer_keys = leaf->buffer_keys;
    const uint64_t* insert_buffer_values = leaf->buffer_values;
#endif
#endif

#ifdef LEAF_V_DMA
    const uint64_t* sorted_keys = (const uint64_t*)(((uintptr_t) hga) + leaf->host_hga_to_sorted_keys_offset);
    const uint64_t* sorted_values = (const uint64_t*)(((uintptr_t) hga) +
        leaf->host_hga_to_sorted_keys_offset + LEARNED_KEYS_PER_LEAF * 8);
#else
    const uint64_t* sorted_keys = leaf->sorted_keys;
    const uint64_t* sorted_values = leaf->sorted_values;
#endif

#ifdef LEAF_INSERTBUFFER
    DPAStoreRangeEntry buffer_entries[LEARNED_INSERT_BUFFER_SIZE];
    uint16_t filtered_buffer = 0;
    for (uint16_t i = 0; i < leaf->buffer_high; ++i) {
        uint64_t bkey = insert_buffer_keys[i];
        if (bkey < start_key) {
            continue;
        }
        buffer_entries[filtered_buffer].key = bkey;
        buffer_entries[filtered_buffer].value = insert_buffer_values[i];
        filtered_buffer++;
    }
    if (filtered_buffer > 1) {
        dpa_range_sort_entries(buffer_entries, filtered_buffer);
    }
#else
    DPAStoreRangeEntry buffer_entries[1];
    uint16_t filtered_buffer = 0;
#endif

    int sorted_idx = 0;
    if (leaf->sorted_count > 0) {

#ifdef USE_BTREE_ALGORITHM
        int low = 0;
        int high = leaf->sorted_count;
        while (low < high) {
            int mid = low + ((high - low) >> 1);
            if (sorted_keys[mid] < start_key) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        sorted_idx = low;
#else
    /* Use model to predict position */
    uint64_t delta = start_key - leaf->first_key;  // guaranteed non-negative by earlier guard
    __uint128_t normalized_key = (__uint128_t)delta;
    __uint128_t product = normalized_key * (__uint128_t)leaf->slope_fp;
    __uint128_t base128 = product >> FIXED_POINT_SHIFT;
    int64_t base64 = (int64_t)base128;
    int64_t sum = base64 + leaf->intercept_fp;
    int predicted = (int)sum;

#ifdef BRANCHLESS
        int sign_mask = predicted >> 31;
        predicted = predicted & ~sign_mask;
        int max_idx = leaf->sorted_count - 1;
        int diff = predicted - max_idx;
        int over_mask = diff >> 31;
        predicted = (predicted & over_mask) | (max_idx & ~over_mask);
#else
        if (__builtin_expect(predicted < 0, 0)) predicted = 0;
        if (__builtin_expect(predicted >= leaf->sorted_count, 0)) predicted = leaf->sorted_count - 1;
#endif

        int traverse_epsilon = EPSILON_TRAVERSE_LEAF;

        int search_start = 0;
        int search_end = leaf->sorted_count;
#ifdef BRANCHLESS
        int start_raw = predicted - traverse_epsilon;
        int start_sign = start_raw >> 31;
        search_start = start_raw & ~start_sign;

        int tentative_end = predicted + traverse_epsilon + 1;
        int end_diff = tentative_end - leaf->sorted_count;
        int end_sign = end_diff >> 31;
        search_end = (tentative_end & end_sign) | (leaf->sorted_count & ~end_sign);
#else
        search_start = 0;
        if (__builtin_expect(predicted >= traverse_epsilon, 1)) search_start = predicted - traverse_epsilon;
        search_end = predicted + traverse_epsilon + 1;
        if (__builtin_expect(search_end > leaf->sorted_count, 0)) search_end = leaf->sorted_count;
#endif

        sorted_idx = search_start;
        while (sorted_idx < search_end && sorted_keys[sorted_idx] < start_key) {
            sorted_idx++;
        }

        if (sorted_idx == search_end) {
            while (sorted_idx < leaf->sorted_count && sorted_keys[sorted_idx] < start_key) {
                sorted_idx++;
            }
        } else {
            while (sorted_idx > 0 && sorted_keys[sorted_idx - 1] >= start_key) {
                sorted_idx--;
            }
        }
#endif
    }

    uint16_t written = 0;
    int sorted_pos = sorted_idx;
    uint16_t buffer_pos = 0;

    while (written < max_entries) {
        bool have_sorted = (sorted_pos < leaf->sorted_count);
        bool have_buffer = (buffer_pos < filtered_buffer);
        if (!have_sorted && !have_buffer) {
            break;
        }

        DPAStoreRangeEntry next_entry;
        if (!have_buffer) {
            next_entry.key = sorted_keys[sorted_pos];
            next_entry.value = sorted_values[sorted_pos];
            sorted_pos++;
        } else if (!have_sorted) {
            next_entry = buffer_entries[buffer_pos++];
        } else {
            uint64_t sorted_key = sorted_keys[sorted_pos];
            uint64_t buffer_key = buffer_entries[buffer_pos].key;
            if (buffer_key <= sorted_key) {
                next_entry = buffer_entries[buffer_pos++];
                if (buffer_key == sorted_key) {
                    sorted_pos++;
                }
            } else {
                next_entry.key = sorted_key;
                next_entry.value = sorted_values[sorted_pos++];
            }
        }

        out_entries[written++] = next_entry;
    }

    bool more_sorted = (sorted_pos < leaf->sorted_count);
    bool more_buffer = (buffer_pos < filtered_buffer);
    if (leaf_exhausted) {
        *leaf_exhausted = !(more_sorted || more_buffer);
    }

    return written;
}

static inline uint16_t dpa_learned_tree_find_range(const DPALearnedTree* tree,
                                                   uint64_t start_key,
                                                   DPAStoreRangeEntry* out_entries,
                                                   uint16_t max_entries,
                                                   host_global_area_t* hga) {
    if (!tree || !tree->root || max_entries == 0) {
        return 0;
    }

    uint16_t total = 0;
    DPALearnedLeafNode* current_leaf = NULL;
    uint64_t current_key = start_key;
    uint64_t sibling_pivot = 0;
    bool sibling_pivot_valid = false;

#if DPASTORE_DEBUG_RANGE
    LOG_I("DPA-RANGE: start key=%lu max=%u\n", start_key, max_entries);
#endif

    while (total < max_entries) {
        if (!current_leaf) {
            DPALearnedTreeNode* node = tree->root;
            sibling_pivot_valid = false;
            while (node && dpa_learned_node_is_inner(node)) {
                node = dpa_learned_inner_find_child_range(&node->inner,
                                                          current_key,
                                                          &sibling_pivot,
                                                          &sibling_pivot_valid);
            }
            if (!node) {
#if DPASTORE_DEBUG_RANGE
                LOG_I("DPA-RANGE: traversal ended before reaching leaf (current_key=%lu)\n", current_key);
#endif
                break;
            }
            current_leaf = &node->leaf;
#if DPASTORE_DEBUG_RANGE
            LOG_I("DPA-RANGE: visiting leaf=%p start_key=%lu pivot_valid=%d pivot=%lu\n",
                  (void*)current_leaf,
                  current_key,
                  sibling_pivot_valid,
                  sibling_pivot);
#endif
        }

        uint16_t start_idx = total;
        bool leaf_exhausted = true;
        uint16_t produced = dpa_learned_leaf_find_range(current_leaf,
                                                        current_key,
                                                        out_entries + start_idx,
                                                        max_entries - total,
                                                        &leaf_exhausted,
                                                        hga);

        if (produced > 0) {
            total += produced;
        }

#if DPASTORE_DEBUG_RANGE
        LOG_I("DPA-RANGE: leaf=%p produced=%u total=%u exhausted=%d\n",
              (void*)current_leaf,
              produced,
              total,
              leaf_exhausted);
#endif

        if (total >= max_entries) {
            break;
        }

        uint64_t next_key_in_leaf = current_key;
        bool have_next_in_leaf = false;
        if (produced > 0) {
            uint16_t last_idx = start_idx + produced - 1;
            uint64_t last_key = out_entries[last_idx].key;
            if (last_key != UINT64_MAX) {
                next_key_in_leaf = last_key + 1;
                have_next_in_leaf = true;
            }
        }

        if (!leaf_exhausted) {
            if (!have_next_in_leaf) {
#if DPASTORE_DEBUG_RANGE
                LOG_I("DPA-RANGE: cannot advance within leaf=%p (last key was UINT64_MAX)\n", (void*)current_leaf);
#endif
                break;
            }
            current_key = next_key_in_leaf;
#if DPASTORE_DEBUG_RANGE
            LOG_I("DPA-RANGE: continue within leaf=%p next_key=%lu\n", (void*)current_leaf, current_key);
#endif
            continue;
        }

        current_leaf = NULL;
        if (!sibling_pivot_valid) {
#if DPASTORE_DEBUG_RANGE
            LOG_I("DPA-RANGE: no sibling pivot available, stopping (total=%u)\n", total);
#endif
            break;
        }
#if DPASTORE_DEBUG_RANGE
        LOG_I("DPA-RANGE: moving to sibling pivot key=%lu\n", sibling_pivot);
#endif
        current_key = sibling_pivot;
        sibling_pivot_valid = false;
    }

#if DPASTORE_DEBUG_RANGE
    LOG_I("DPA-RANGE: completed total=%u\n", total);
#endif
    return total;
}

/* Tree traversal - find value for given key */
static inline bool dpa_learned_tree_find(const DPALearnedTree* tree, uint64_t key, uint64_t* value,
                                         host_global_area_t* hga) {
    DPALearnedTreeNode* current = tree->root;

    // /* Prefetch root's segment keys if it's an inner node */
    // if (dpa_learned_node_is_inner(current)) {
         //__builtin_prefetch(&current->inner.segment_first_keys[0], 0, 3);
    // }

    /* Traverse down the tree - optimized for inner nodes being more common */
    while (current) {
        /* Check inner first (more common in upper levels) */
        if (__builtin_expect(dpa_learned_node_is_inner(current), 1)) {
            /* At inner node, find appropriate child */
            DPALearnedTreeNode* next = dpa_learned_inner_find_child(&current->inner, key);
            /* Prefetch next node - this gets header and first segment keys */
            __builtin_prefetch(next, 0, 0);
            current = next;
        } else {
            return dpa_learned_leaf_find(&current->leaf, key, value, hga);
        }
    }
    
    return false;
}

/* Result codes for insert operations */
typedef enum {
    DPA_INSERT_SUCCESS = 0,
    DPA_INSERT_BUFFER_FULL = 1,        /* Buffer full, need to send patch */
    DPA_INSERT_UPDATE_IN_PROGRESS = 2, /* Buffer full, patch already sent, waiting for update */
    DPA_INSERT_ERROR = 3                /* Generic error */
} dpa_insert_result_t;

/* Insert key into leaf node buffer */
static inline dpa_insert_result_t dpa_learned_leaf_insert(DPALearnedLeafNode* leaf, uint64_t key, uint64_t value) {

#if DEBUG_INSERTS_DPU_SIDE_FENCE_ATOMICS == 1
    //__dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW);
#endif

    /* Real buffer full condition */
    /* Check if update is already in progress */
    if (leaf->update_in_progress) {
        // LOG_I("DPA: leaf_insert - buffer full but update already in progress, waiting for update, key %lu\n", key);
#if DEBUG_INSERTS_DPU_SIDE_FENCE_ATOMICS == 1
        //dpa_spinlock_release(&leaf->buffer_lock);
#endif
        return DPA_INSERT_UPDATE_IN_PROGRESS;  /* Update pending, retry from root */
    }

    /* Acquire spinlock for buffer access */
    dpa_spinlock_acquire(&leaf->buffer_lock);

    // /* Check if key already exists in insert buffer */
    // for (uint16_t i = 0; i < leaf->buffer_high; i++) {
    //     if (leaf->buffer_keys[i] == key) {
    //         /* Update existing key */
    //         leaf->buffer_values[i] = value;
    //         dpa_spinlock_release(&leaf->buffer_lock);
    //         return DPA_INSERT_SUCCESS;
    //     }
    // }
    
    /* Add new key to insert buffer if space available */
    if (leaf->buffer_high < LEARNED_INSERT_BUFFER_SIZE) {

        uint16_t slot = leaf->buffer_high;
        leaf->buffer_keys[slot] = key;
        leaf->buffer_values[slot] = value;
        leaf->buffer_high++;
        //leaf->insert_count_total++;

        if (leaf->buffer_high == LEARNED_INSERT_BUFFER_SIZE) {
            leaf->update_in_progress = 1;
#ifdef DEBUG_INSERTS_DPU_SIDE_FENCE_ATOMICS_2 == 1
            __dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW);
#endif
            dpa_spinlock_release(&leaf->buffer_lock);
            return DPA_INSERT_BUFFER_FULL;
        }
#ifdef DEBUG_INSERTS_DPU_SIDE_FENCE_ATOMICS_2 == 1
        __dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW);
#endif
        dpa_spinlock_release(&leaf->buffer_lock);
        return DPA_INSERT_SUCCESS;
    }

    /* Real buffer full condition */
    /* Check if update is already in progress */
    if (leaf->update_in_progress) {
        // LOG_I("DPA: leaf_insert - buffer full but update already in progress, waiting for update, key %lu\n", key);
        dpa_spinlock_release(&leaf->buffer_lock);
        return DPA_INSERT_UPDATE_IN_PROGRESS;  /* Update pending, retry from root */
    }
    
    /* Set update_in_progress flag atomically while we hold the lock */
    leaf->update_in_progress = 1;
    __dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW);
    // LOG_I("DPA: leaf_insert - buffer full, cannot insert key %lu, setting update_in_progress\n", key);
    dpa_spinlock_release(&leaf->buffer_lock);
    return DPA_INSERT_UPDATE_IN_PROGRESS;
}

/* Insert key-value pair into learned tree */
static inline dpa_insert_result_t dpa_learned_tree_insert(const DPALearnedTree* tree, uint64_t key, uint64_t value, DPALearnedLeafNode** out_leaf) {
    // if (!tree || !tree->root) {
    //     LOG_I("DPA: tree_insert - tree or root is NULL\n");
    //     return DPA_INSERT_ERROR;
    // }
    
    //LOG_I("DPA: tree_insert - inserting key=%lu, value=%lu\n", key, value);
    DPALearnedTreeNode* current = tree->root;
    
    /* Traverse down to find appropriate leaf */
    while (current) {
        if (dpa_learned_node_is_leaf(current)) {
            /* At leaf, attempt to insert into buffer */
            if (out_leaf) *out_leaf = &current->leaf;
            return dpa_learned_leaf_insert(&current->leaf, key, value);
        } else if (dpa_learned_node_is_inner(current)) {
            /* At inner node, find appropriate child */
            //LOG_I("DPA: tree_insert - traversing inner node %p for key=%lu\n", current, key);
            current = dpa_learned_inner_find_child(&current->inner, key);
        } else {
            /* Invalid node type */
            //LOG_I("DPA: tree_insert - invalid node type at %p, type=%d\n", current, current->base.type);
            return DPA_INSERT_ERROR;
        }
    }
    
    //LOG_I("DPA: tree_insert - failed to find leaf node\n");
    return DPA_INSERT_ERROR;
}

/* Initialize DPA learned tree structure */
static inline void dpa_learned_tree_init(DPALearnedTree* tree) {
    if (!tree) return;
    
    tree->root = NULL;
    dpa_spinlock_init(&tree->root_replace_lock);
    dpa_spinlock_init(&tree->debug_lock);
}

/* Send patch for full buffer to host */
static inline bool dpa_learned_send_buffer_full_patch(
    DPALearnedLeafNode* leaf,
    uint32_t thread_id,
    host_global_area_t* cached_hga,
    flexio_uintptr_t host_global_area_addr,
    uint32_t host_global_window_id,
    uint32_t host_global_mkey,
    uint32_t periodic_count) {

    // if (!leaf || host_global_area_addr == 0 || host_global_window_id == 0) {
    //     return false;
    // }

    // LOG_I("DPA: Preparing buffer full patch (thread=%u, leaf=%p, host_leaf=%p, buffer_high=%u)\n",
    //       thread_id,
    //       leaf,
    //       leaf->host_ptr,
    //       leaf->buffer_high);

    /* Use cached host global area pointer when available */
    host_global_area_t *hga = cached_hga;
    
    /* Create patch command with full buffer */
    const uint32_t patch_size = sizeof(command_patch_buffer_full_t);
    command_patch_buffer_full_t *patch_cmd = NULL;

    while (!dpa_patch_acquire(hga, thread_id, patch_size, (void **)&patch_cmd)) {
        /* Spin until space becomes available in the patch queue */
    }

    patch_cmd->size = patch_size;
    patch_cmd->type = CMD_TYPE_PATCH_BUFFER_FULL;
    patch_cmd->thread_id = thread_id;
    patch_cmd->dpa_leaf_ptr = (void*)leaf;
    patch_cmd->host_leaf_ptr = leaf->host_ptr;
    patch_cmd->first_key = leaf->first_key;
    patch_cmd->periodic_count = periodic_count;
    patch_cmd->reserved = 0;

    // patch_cmd->generation = leaf->generation;
    // patch_cmd->insert_count_total = leaf->insert_count_total;

    /* Copy buffer contents with spinlock held */
    //dpa_spinlock_acquire(&leaf->buffer_lock);

    //#define PATCH_SEND_DEBUGGING
    #ifdef PATCH_SEND_DEBUGGING
    if (leaf->buffer_high > 0) {
        uint64_t min_key = leaf->buffer_keys[0];
        for (uint32_t i = 1; i < leaf->buffer_high; i++) {
            uint64_t key = leaf->buffer_keys[i];
            if (key < min_key) {
                min_key = key;
            }
        }

        if (min_key < leaf->first_key /* || min_key % 200 == 0 */) {
            LOG_I("DPA: PATCH_SEND_DEBUG leaf=%p host_leaf=%p buffer_high=%u first_key=%lu buffer_min_key=%lu\n",
                  leaf,
                  leaf->host_ptr,
                  leaf->buffer_high,
                  leaf->first_key,
                  min_key);
            for (uint32_t i = 0; i < leaf->buffer_high; i++) {
                LOG_I("DPA:   Patch entry[%u] key=%lu value=%lu\n",
                    i,
                    leaf->buffer_keys[i],
                    leaf->buffer_values[i]);
            }
        }


    }
    #endif // PATCH_SEND_DEBUGGING


    patch_cmd->buffer_count = leaf->buffer_high;
    for (uint32_t i = 0; i < LEARNED_INSERT_BUFFER_SIZE; i++) {
        patch_cmd->buffer_entries[i].key = leaf->buffer_keys[i];
        patch_cmd->buffer_entries[i].value = leaf->buffer_values[i];
    }
    //dpa_spinlock_release(&leaf->buffer_lock);

    // LOG_I("DPA: Patch payload prepared (leaf=%p, entries=%u)\n",
    //       leaf,
    //       patch_cmd->buffer_count);



#if DEBUG_INSERTS_DPU_SIDE_FENCE_ATOMICS == 1
    __dpa_thread_fence(__DPA_MMIO, __DPA_RW, __DPA_RW);
#endif

    /* Make the populated patch visible to the host */
    dpa_patch_commit(hga, thread_id, patch_cmd, patch_cmd->size);

    // LOG_I("DPA: Successfully sent buffer full patch to host (thread=%u, leaf=%p)\n",
    //       thread_id,
    //       leaf);

    /* Write back to ensure data is visible to host */
    __dpa_thread_fence(__DPA_MMIO, __DPA_RW, __DPA_RW);
    
    return true;
}

#endif /* DPA_LEARNED_TREE_OPS_H */
