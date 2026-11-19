#ifndef TREE_TYPES_H
#define TREE_TYPES_H

#include "common/dpastore_common.h"
#include "../shared/server_config.h"
#include "dpa_learned_tree_types.h"

// Minimal definitions for server packet handling
// The new tree implementation will be added later

// For now, just use the client packet for the server
typedef DPAStoreClientPacket DPAStorePacket;

// Global area with tree support
typedef struct DPAStoreGlobalArea {
    /* DPA learned tree structure and metadata */
    DPALearnedTree tree;            // DPA learned tree
#if NUM_STITCH_QUEUES > 0
    uint64_t stitcher_epochs[NUM_STITCH_QUEUES];
#else
    uint64_t volatile stitcher_epochs[1];
#endif

    /* Reserved space for future use */
    uint64_t reserved[256];         // Reserved for future use
} DPAStoreGlobalArea;

#endif // TREE_TYPES_H
