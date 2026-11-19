#ifndef COMMANDS_H
#define COMMANDS_H

#include <stdint.h>

#include "tree_config.h"

#define CACHE_LINE_SIZE 64

/* Enumeration of command types for runtime type identification */
typedef enum {
    CMD_TYPE_GENERIC = 0,  /* Basic command with no payload */
    CMD_TYPE_TEST = 1,      /* Test command with 64-byte payload */
    CMD_TYPE_PATCH = 2,     /* Generic patch command from DPA reader thread */
    CMD_TYPE_STITCH = 3,    /* Generic stitch command for structural operations */
    CMD_TYPE_STITCH_COPY_LEARNED_LEAF = 4,    /* Stitch command to copy learned leaf node to DPA */
    CMD_TYPE_STITCH_COPY_LEARNED_INNER = 5,   /* Stitch command to copy learned inner node to DPA */
    CMD_TYPE_STITCH_UPDATE_LEARNED_ROOT = 6,      /* Stitch command to update learned root pointer on DPA */
    CMD_TYPE_STITCH_UPDATE_LEARNED_SUBTREE = 7,  /* Stitch command to atomically update a parent->child link */
    CMD_TYPE_STITCH_UNFLAG_LEAF = 8,            /* Stitch command to clear leaf update state */
    CMD_TYPE_PATCH_BUFFER_FULL = 9,             /* Patch command when insert buffer is full */
    CMD_TYPE_STITCH_SET_EPOCH = 10,             /* Stitch command to advance stitcher epoch */
} command_type_t;

/* Base command structure - minimal command with just size and type
 * packed attribute ensures no padding between fields
 * Size field must match sizeof(entire command struct) for ring buffer */
typedef struct __attribute__((packed)) {
    uint32_t size;  /* Total size of this command structure */
    uint32_t type;  /* Command type from command_type_t enum */
} command_generic_t;

/* Test command with additional data payload
 * All commands are self-contained - no pointers allowed
 * This ensures commands can be safely copied between host/DPA memory */
typedef struct __attribute__((packed)) {
    uint32_t size;       /* Total size of this command structure */
    uint32_t type;       /* CMD_TYPE_TEST */
    uint8_t data[56];    /* Test payload data */
} command_test_t;

/* Patch command - generic patch command from DPA reader thread to host
 * For testing purposes, just contains test data like command_test_t */
typedef struct __attribute__((packed)) {
    uint32_t size;       /* Total size of this command structure */
    uint32_t type;       /* CMD_TYPE_PATCH */
    uint32_t thread_id;  /* Reader thread ID (0 to NUM_PATCH_QUEUES-1) */
    uint8_t data[52];    /* Test payload data */
} command_patch_t;

/* Stitch command - generic stitch command for structural operations
 * For testing purposes, just contains test data */
typedef struct __attribute__((packed)) {
    uint32_t size;       /* Total size of this command structure */
    uint32_t type;       /* CMD_TYPE_STITCH */
    uint8_t data[56];    /* Test payload data */
} command_stitch_t;

/* Include tree configuration for node size macros */
#include "tree_config.h"

/* Stitch command to copy a learned leaf node to DPA memory
 * Contains sorted entries and model parameters */
typedef struct __attribute__((packed)) {
    uint32_t size;          /* Total size of this command structure */
    uint32_t type;          /* CMD_TYPE_STITCH_COPY_LEARNED_LEAF */
    void* dpa_ptr;          /* Target DPA memory pointer */
    void* host_ptr;         /* Host pointer to store in DPA node */
    uint32_t sorted_count;  /* Number of sorted entries */
    uint32_t padding;       /* Padding for alignment */
    uint64_t slope_fp;      /* Fixed-point slope for model */
    int64_t intercept_fp;   /* Fixed-point intercept for model */
    uint64_t first_key;     /* First key in sorted area */
    uint64_t UUID_version;
#ifndef USE_MICRO_LEAVES
    struct {
        uint64_t key;
        uint64_t value;
    } sorted_entries[LEARNED_KEYS_PER_LEAF];  /* Sorted KV pairs */
#endif //USE_MICRO_LEAVES
} command_stitch_copy_learned_leaf_t;

/* Learned segment info for inner nodes */
typedef struct __attribute__((packed)) {
    uint64_t first_key;     /* First key this segment covers */
    uint64_t slope_fp;      /* Fixed-point slope */
    int64_t intercept_fp;   /* Fixed-point intercept */
    uint16_t first_child_idx; /* Index of first child */
    uint16_t num_children;  /* Number of children covered */
    uint32_t padding;       /* Alignment padding */
} stitch_learned_segment_t;

/* Stitch command to copy a learned inner node to DPA memory
 * Contains segments and child pointers */
typedef struct __attribute__((packed)) {
    uint32_t size;          /* Total size of this command structure */
    uint32_t type;          /* CMD_TYPE_STITCH_COPY_LEARNED_INNER */
    void* dpa_ptr;          /* Target DPA memory pointer */
    uint16_t num_segments;  /* Number of segments */
    uint16_t num_children;  /* Number of children */
    uint8_t tree_level;     /* Level in tree (root = 0) */
    uint8_t padding[3];     /* Padding for alignment */
    uint64_t min_key;       /* Minimum key in subtree */
    //uint64_t max_key_DEPRECATED;       /* Maximum key in subtree */
    uint64_t UUID_version;
    stitch_learned_segment_t segments[LEARNED_SEGMENTS_PER_INNER];  /* Segments array */
    uint64_t child_first_keys[LEARNED_SEGMENTS_PER_INNER * LEARNED_KEYS_PER_INNER_SEGMENT];  /* First key of each child for efficient traversal */
    void* children[LEARNED_SEGMENTS_PER_INNER * LEARNED_KEYS_PER_INNER_SEGMENT];    /* Child pointers */
} command_stitch_copy_learned_inner_t;

/* Stitch command to update the learned root pointer on DPA */
typedef struct __attribute__((packed)) {
    uint32_t size;          /* Total size of this command structure */
    uint32_t type;          /* CMD_TYPE_STITCH_UPDATE_LEARNED_ROOT */
    void* new_root;         /* New root node address */
    uint64_t expected_UUID_version;
    uint64_t barrier_epoch; /* Epoch that must be observed before applying root */
    uint16_t reserved[2];
} command_stitch_update_learned_root_t;

/* Stitch command to patch a single parent->child connection in the learned tree */
typedef struct __attribute__((packed)) {
    uint32_t size;          /* Total size of this command structure */
    uint32_t type;          /* CMD_TYPE_STITCH_UPDATE_LEARNED_SUBTREE */
    void* parent_ptr;       /* DPA pointer to parent node being updated */
    void* new_child_ptr;    /* DPA pointer to new child subtree */
    uint16_t child_index;   /* Which child slot to update */
    uint16_t reserved[2];      /* Alignment */
    uint16_t stitch_into_root;
    uint64_t expected_UUID_version;
    uint64_t expected_first_key;
} command_stitch_update_learned_subtree_t;

typedef struct __attribute__((packed)) {
    uint32_t size;          /* Total size of this command structure */
    uint32_t type;          /* CMD_TYPE_STITCH_UNFLAG_LEAF */
    void* dpa_leaf_ptr;     /* DPA pointer to the leaf to clear */
    void* host_leaf_ptr;    /* Host pointer for diagnostics */
} command_stitch_unflag_leaf_t;

/* Stitch command to publish a barrier epoch to stitcher threads */
typedef struct __attribute__((packed)) {
    uint32_t size;          /* Total size of this command structure */
    uint32_t type;          /* CMD_TYPE_STITCH_SET_EPOCH */
    uint64_t epoch;         /* Epoch value to publish */
} command_stitch_set_epoch_t;

/* Patch command when insert buffer is full
 * Sent from DPA to host containing the full buffer that needs merging */
typedef struct __attribute__((packed)) {
    uint32_t size;          /* Total size of this command structure */
    uint32_t type;          /* CMD_TYPE_PATCH_BUFFER_FULL */
    uint32_t thread_id;     /* Reader thread ID that detected full buffer */
    uint32_t buffer_count;  /* Number of entries in buffer */
    void* dpa_leaf_ptr;     /* DPA pointer of the leaf node with full buffer */
    void* host_leaf_ptr;    /* Host pointer from DPA node */

    uint64_t first_key;
    uint32_t periodic_count; /* Reader periodic counter snapshot */
    uint32_t reserved;
    // uint32_t insert_count_total;
    // uint32_t generation;
    uint64_t padding[2];
    struct {
        uint64_t key;
        uint64_t value;
    } buffer_entries[LEARNED_INSERT_BUFFER_SIZE];  /* The full insert buffer contents */
} command_patch_buffer_full_t;

#if defined(__cplusplus)
static_assert(sizeof(command_patch_buffer_full_t) == 320,
              "HostLearnedLeafNode size must be 320");
#else
_Static_assert(sizeof(command_patch_buffer_full_t) == 320,
               "HostLearnedLeafNode size must be 320");
#endif

#endif /* COMMANDS_H */
