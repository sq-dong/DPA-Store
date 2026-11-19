#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

#include <stdint.h>

/* Include common config for thread counts */
#include "common/dpastore_common_config.h"

/* Host Global Area Configuration */
/* Node sizes (must match actual struct sizes) */
#define HOST_LEAF_NODE_SIZE       2432 //  896 for 32 // 1408 for 64 // 2432 for 128 // 4480 for 256                 /* sizeof(HostLearnedLeafNode) */
#define HOST_INNER_NODE_SIZE      7488 // 2112 for 32 // 3904 for 64 // 7488 for 128 // 14656 for 256                /* sizeof(HostLearnedInnerNode) */

#define DPASTORE_STITCHER_THREAD_COUNT 4
#ifndef DPASTORE_STITCHER_THREAD_COUNT
#define DPASTORE_STITCHER_THREAD_COUNT 4
#endif

#define HOST_TREE_NUM_PARTITIONS  4
#define STITCHING_RACE_BARRIERS 1

/* Pool sizes - must be multiples of node sizes */
#define HOST_TREE_LEAF_POOL_SIZE  (4166ULL * HOST_LEAF_NODE_SIZE * 69ULL * 11)         /* ≈ 667 MiB */
#define HOST_TREE_INNER_POOL_SIZE (700ULL * HOST_INNER_NODE_SIZE * 30ULL * 20)         /* ≈ 295 MiB */

#ifndef TREE_UPDATE_STATS_FOR_FIRST
//#define TREE_UPDATE_STATS_FOR_FIRST 25000000ULL
#endif

/* DPA Tree Configuration */
/* DPA node sizes (must match actual struct sizes) */
#define USE_MICRO_LEAVES
#ifdef USE_MICRO_LEAVES
#define DPA_LEAF_NODE_SIZE                320          /* sizeof(DPALearnedLeafNode) without inline sorted KV storage */
#define DPA_TREE_LEAF_POOL_NODE_COUNT     30511ULL     /* Scaled to keep total leaf pool size roughly constant */
#else
#define DPA_LEAF_NODE_SIZE                2368         /* sizeof(DPALearnedLeafNode) */
#define DPA_TREE_LEAF_POOL_NODE_COUNT     4123ULL
#endif
#define DPA_INNER_NODE_SIZE       14656 // 3904 for 32 // 7488 for 64 // 14656 for 128 // 28992 for 256                /* sizeof(DPALearnedInnerNode) */

/* DPA tree pool sizes - allocated on DPA device memory */
#define DPA_TREE_LEAF_POOL_SIZE   (DPA_TREE_LEAF_POOL_NODE_COUNT * DPA_LEAF_NODE_SIZE * 69ULL)   /* Default: 4123 * 2432 ≈ 10MB */
#define DPA_TREE_INNER_POOL_SIZE  (344ULL * DPA_INNER_NODE_SIZE * 30ULL)   /* 344 * 28992 = ~10MB */
#define DPA_TREE_TOTAL_SIZE       (DPA_TREE_LEAF_POOL_SIZE + DPA_TREE_INNER_POOL_SIZE)

/* Patcher configuration */
#define DPASTORE_PATCHER_THREAD_COUNT 4
#ifndef DPASTORE_PATCHER_THREAD_COUNT
#define DPASTORE_PATCHER_THREAD_COUNT 4
#endif

#if DPASTORE_PATCHER_THREAD_COUNT > 1
#define DPASTORE_PATCH_PATH_NEEDS_LOCKS 1
#else
#define DPASTORE_PATCH_PATH_NEEDS_LOCKS 0
#endif

/* Queue Configuration - DPA to Host communication */
#define NUM_PATCH_QUEUES          DPASTORE_READER_THREAD_COUNT  /* One patch queue per reader thread */
#define PATCH_QUEUE_SIZE          (8ULL * 1024 * 1024)                   /* 32KB per patch queue */
#define STITCH_QUEUE_SIZE         (128ULL * 1024 * 1024)            /* 64MB per stitch queue */
#define NUM_STITCH_QUEUES         DPASTORE_STITCHER_THREAD_COUNT

// /* Total size includes all components */
// #define HOST_GLOBAL_AREA_SIZE     (HOST_TREE_LEAF_POOL_SIZE + HOST_TREE_INNER_POOL_SIZE + \
//                                   (NUM_PATCH_QUEUES * PATCH_QUEUE_SIZE) + STITCH_QUEUE_SIZE + \
//                                   (8 * 1024))  /* 8KB for metadata and padding */

/* Cache line size for alignment */
#define CACHE_LINE_SIZE           64

// #define DEBUG_INSERTS 0
#define DEBUG_INSERTS_DPU_SIDE_FENCE_ATOMICS 1
#define DEBUG_INSERTS_DPU_SIDE_FENCE_ATOMICS_2 1
#define DEBUG_INSERTS_DPU_SIDE_STITCHING 0
#define DEBUG_INSERTS_MULTI_PATCHER 0

#endif /* SERVER_CONFIG_H */
