#ifndef DPASTORE_GLOBAL_AREA_H
#define DPASTORE_GLOBAL_AREA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "common/dpastore_common.h"
#include "tree_types.h"
#include "dpa_learned_tree_ops.h"

// Function to initialize the global area
void global_area_init()
{
    DPAStoreGlobalArea* global = (DPAStoreGlobalArea*)global_area_addr;
    if (!global) {
        DEV_LOG_ERROR("DPA: global_area_addr is not set, cannot initialize global area.\n");
        return;
    }

    // Initialize tree to empty state (will be populated by stitch commands)
    dpa_learned_tree_init(&global->tree);

#if NUM_STITCH_QUEUES > 0
    for (uint32_t idx = 0; idx < NUM_STITCH_QUEUES; ++idx) {
        global->stitcher_epochs[idx] = 0;
    }
#else
    global->stitcher_epochs[0] = 0;
#endif

    LOG_I("DPA: Global area initialized successfully.\n");
}

#endif // DPASTORE_GLOBAL_AREA_H
