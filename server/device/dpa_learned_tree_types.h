#ifndef DPA_LEARNED_TREE_TYPES_H
#define DPA_LEARNED_TREE_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "../shared/tree_config.h"

#ifdef __DEVICE__
#include "spinlock.h"
#else
/* Define spinlock type for host compilation */
typedef uint32_t dpa_spinlock_t;
#endif

/* DPA-side structures for the learned tree from host bulk load
 * These match the host learned tree structure but are C-compatible */

/* Key-Value pair structure (matches host KVPair) */
typedef struct {
    uint64_t key;
    uint64_t value;
} DPAKVPair;

/* Base node type enumeration */
typedef enum {
    DPA_LEARNED_LEAF = 0,
    DPA_LEARNED_INNER = 1
} DPALearnedNodeType;

/* DPA Learned Leaf Node - stores sorted entries with learned model and insert buffer */
typedef struct {
    uint8_t type;                                        /* Node type (DPA_LEARNED_LEAF) */
    uint8_t update_in_progress;                          /* Flag to prevent multiple patches */
    uint8_t padding1[2];
    dpa_spinlock_t buffer_lock;                          /* Spinlock for buffer access */

    uint64_t first_key;                                  /* First key in sorted area */
    //uint64_t last_key_DEPRECATED;                                   /* Last key in sorted area */

    uint64_t UUID_version;

    //uint32_t insert_count_total;
    //uint32_t generation;

    uint16_t buffer_high;                                /* High water mark for buffer entries */
    uint16_t sorted_count;                               /* Number of sorted entries */

    uint8_t padding3[4];

    /* Model parameters */
    uint64_t slope_fp;                                   /* Fixed-point slope */
    int64_t intercept_fp;                                /* Fixed-point intercept */

    /* Host reference for patching */
    void* host_ptr;                                      /* Pointer to corresponding host leaf node */

    /* Offset from host global area base to sorted_entries in host leaf */
    uint64_t host_hga_to_sorted_keys_offset;                 /* Offset to directly access host sorted_entries from DPA */

    /* Key-value separated insert buffer for fast key scanning */
    /* Keys are contiguous for 1-2 cache line access during search */
    uint64_t buffer_keys[16];                            /* Buffer keys (max 16, fits in 2 cache lines) */
    uint64_t buffer_values[16];                          /* Buffer values (accessed after key match) */

#ifndef USE_MICRO_LEAVES
    /* Key-value separated sorted area for cache-efficient scanning (local storage fallback) */
    uint64_t sorted_keys[LEARNED_KEYS_PER_LEAF];        /* Sorted keys (128 entries) */
    uint64_t sorted_values[LEARNED_KEYS_PER_LEAF];      /* Sorted values (accessed after key match) */
#endif
    
} DPALearnedLeafNode;

/* Learned segment structure for inner nodes */
typedef struct {
    uint64_t first_key;         /* First key this segment covers */
    uint64_t slope_fp;          /* Fixed-point slope */
    int64_t intercept_fp;       /* Fixed-point intercept */
    uint16_t first_child_idx;   /* Index of first child */
    uint16_t num_children;      /* Number of children covered */
    uint32_t padding;           /* Alignment padding */
} DPALearnedSegment;

/* DPA Learned Inner Node - contains segments and child pointers */
typedef struct {
    uint8_t type;                                             /* Node type (DPA_LEARNED_INNER) */
    uint8_t tree_level;                                       /* Level in tree (root = 0) */
    uint16_t num_segments;                                    /* Actual segments used */
    uint16_t num_children;                                    /* Actual children */
    uint16_t padding;                                         /* Padding for alignment */

    /* First keys array - all 7 keys fit in 1st cache line (64 bytes) */
    /* This allows finding the correct segment with a single memory access */
    uint64_t segment_first_keys[LEARNED_SEGMENTS_PER_INNER];  /* First key of each segment */

    /* Segments for routing */
    DPALearnedSegment segments[LEARNED_SEGMENTS_PER_INNER];   /* Segments array */

    /* Key range */
    uint64_t min_key;                                         /* Minimum key in subtree */
    //uint64_t max_key_DEPRECATED;                                         /* Maximum key in subtree */
    uint64_t UUID_version;

    uint8_t padding1[16];

    /* Child first keys - for efficient traversal without pointer dereferencing */
    uint64_t child_first_keys[LEARNED_SEGMENTS_PER_INNER * LEARNED_KEYS_PER_INNER_SEGMENT];  /* First key of each child */

    /* Child pointers - maximum children = segments * keys_per_segment */
    void* children[LEARNED_SEGMENTS_PER_INNER * LEARNED_KEYS_PER_INNER_SEGMENT];  /* Children array */


} DPALearnedInnerNode;

/* Union type for generic node access */
typedef union {
    struct {
        uint8_t type;                        /* First byte is always node type */
        uint8_t padding[7];
    } base;
    DPALearnedLeafNode leaf;
    DPALearnedInnerNode inner;
} DPALearnedTreeNode;

/* DPA learned tree */
typedef struct {
    DPALearnedTreeNode* root;               /* Root of the DPA learned tree */
    dpa_spinlock_t root_replace_lock;
    dpa_spinlock_t debug_lock;
} DPALearnedTree;

#if defined(__cplusplus)
static_assert(sizeof(DPALearnedLeafNode) == DPA_LEAF_NODE_SIZE,
              "DPA_LEAF_NODE_SIZE must match DPALearnedLeafNode when USE_MICRO_LEAVES toggles");
#else
_Static_assert(sizeof(DPALearnedLeafNode) == DPA_LEAF_NODE_SIZE,
               "DPA_LEAF_NODE_SIZE must match DPALearnedLeafNode when USE_MICRO_LEAVES toggles");
#endif

#endif /* DPA_LEARNED_TREE_TYPES_H */
