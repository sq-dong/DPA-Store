#define DOCA_ALLOW_EXPERIMENTAL_API

#include "host_global_area_mgmt.h"
#include <stdlib.h>
#include <stddef.h>
#include <infiniband/verbs.h>
#include <doca_log.h>

DOCA_LOG_REGISTER(HOST_GLOBAL_AREA);

/* Initialize host global area in provided memory
 * Memory must be aligned to 64 bytes (cache line) and registered with ibv_reg_mr
 * @param hga: Pointer to host global area structure
 * @param memory: Aligned memory region for the entire global area
 * @param total_size: Total size of memory region
 * @return: 1 on success, 0 on failure */
int host_global_area_init(
    host_global_area_t *hga,
    void *memory,
    uint64_t total_size)
{
    if (!hga || !memory || total_size < sizeof(host_global_area_t)) {
        DOCA_LOG_ERR("Invalid parameters for host_global_area_init");
        return 0;
    }
    
    /* The host_global_area_t structure is already at the memory location */
    /* We just need to initialize the metadata fields */
    
    DOCA_LOG_INFO("host_global_area_init: sizeof(host_global_area_t)=%lu", sizeof(host_global_area_t));
    
    /* Initialize patch queue metadata - one per reader thread */
    for (int i = 0; i < NUM_PATCH_QUEUES; i++) {
        if (!ringbuffer_init(&hga->patch_queue_metadata[i], PATCH_QUEUE_SIZE)) {
            DOCA_LOG_ERR("Failed to initialize patch queue %d metadata", i);
            return 0;
        }
    }
    
    /* Initialize stitch queue metadata */
    for (int i = 0; i < NUM_STITCH_QUEUES; i++) {
        if (!ringbuffer_init(&hga->stitch_queue_metadata[i], STITCH_QUEUE_SIZE)) {
            DOCA_LOG_ERR("Failed to initialize stitch queue metadata %d", i);
            return 0;
        }
    }
    
    /* Initialize flags and counters */
    hga->flags = 0;
    hga->patches_sent = 0;
    hga->patches_received = 0;
    hga->stitches_sent = 0;
    hga->stitches_received = 0;
    
    DOCA_LOG_INFO("host_global_area_init completed successfully");
    DOCA_LOG_INFO("  Patch queues: %u queues of %llu KB each", NUM_PATCH_QUEUES, PATCH_QUEUE_SIZE / 1024);
    DOCA_LOG_INFO("  Stitch queue: %llu KB", STITCH_QUEUE_SIZE / 1024);
    DOCA_LOG_INFO("  Tree leaf pool: %u MB", (unsigned int)(HOST_TREE_LEAF_POOL_SIZE / (1024 * 1024)));
    DOCA_LOG_INFO("  Tree inner pool: %u MB", (unsigned int)(HOST_TREE_INNER_POOL_SIZE / (1024 * 1024)));
    
    return 1;
}

/* Allocate aligned memory suitable for DPA window access
 * Memory must be 64-byte aligned for window operations
 * @param size: Size of memory to allocate
 * @return: Pointer to aligned memory or NULL on failure */
void* host_global_area_alloc_memory(uint64_t size)
{
    void *memory = NULL;
    
    /* Align size to cache line */
    size = align_to_cache_line(size);
    
    /* Use posix_memalign for 64-byte alignment as required by DPA windows */
    if (posix_memalign(&memory, CACHE_LINE_SIZE, size) != 0) {
        return NULL;
    }
    
    return memory;
}

/* Free memory allocated by host_global_area_alloc_memory
 * @param memory: Memory to free */
void host_global_area_free_memory(void *memory)
{
    if (memory) {
        free(memory);
    }
}

/* Register memory region for DPA access
 * @param pd: Protection domain
 * @param memory: Memory region to register
 * @param size: Size of memory region
 * @return: Memory region handle or NULL on failure */
struct ibv_mr* host_global_area_register_memory(
    struct ibv_pd *pd,
    void *memory,
    uint64_t size)
{
    if (!pd || !memory || size == 0) {
        return NULL;
    }
    
    /* Register memory with appropriate access flags for DPA window */
    int access_flags = IBV_ACCESS_LOCAL_WRITE | 
                      IBV_ACCESS_REMOTE_READ | 
                      IBV_ACCESS_REMOTE_WRITE;
    
    return ibv_reg_mr(pd, memory, size, access_flags);
}

/* Unregister memory region
 * @param mr: Memory region to unregister
 * @return: 0 on success, non-zero on failure */
int host_global_area_unregister_memory(struct ibv_mr *mr)
{
    if (!mr) {
        return -1;
    }
    
    return ibv_dereg_mr(mr);
}

/* Create and initialize a complete host global area
 * This is a convenience function that allocates, registers, and initializes
 * @param pd: Protection domain for memory registration
 * @param total_size: Total size for the global area
 * @param hga_out: Output - pointer to initialized host global area
 * @param mr_out: Output - memory region handle
 * @return: 1 on success, 0 on failure */
int host_global_area_create(
    struct ibv_pd *pd,
    uint64_t total_size,
    host_global_area_t **hga_out,
    struct ibv_mr **mr_out)
{
    void *memory = NULL;
    struct ibv_mr *mr = NULL;
    
    if (!pd || !hga_out || !mr_out) {
        DOCA_LOG_ERR("Invalid parameters");
        return 0;
    }
    
    DOCA_LOG_INFO("Allocating %lu bytes of aligned memory", total_size);
    
    /* Allocate aligned memory */
    memory = host_global_area_alloc_memory(total_size);
    if (!memory) {
        DOCA_LOG_ERR("Failed to allocate aligned memory");
        return 0;
    }
    
    DOCA_LOG_INFO("Memory allocated at %p", memory);
    DOCA_LOG_INFO("Registering memory with protection domain");
    
    /* Register memory for DPA access */
    mr = host_global_area_register_memory(pd, memory, total_size);
    if (!mr) {
        DOCA_LOG_ERR("Failed to register memory");
        host_global_area_free_memory(memory);
        return 0;
    }
    
    DOCA_LOG_INFO("Memory registered - lkey: 0x%x, rkey: 0x%x", 
           mr->lkey, mr->rkey);
    DOCA_LOG_INFO("Initializing queues - %d patch queues, %d stitch queue", NUM_PATCH_QUEUES, NUM_STITCH_QUEUES);
    
    /* Initialize the host global area */
    host_global_area_t *hga = (host_global_area_t *)memory;
    if (!host_global_area_init(hga, memory, total_size)) {
        DOCA_LOG_ERR("Failed to initialize host global area");
        host_global_area_unregister_memory(mr);
        host_global_area_free_memory(memory);
        return 0;
    }
    
    DOCA_LOG_INFO("Successfully created host global area");
    
    *hga_out = hga;
    *mr_out = mr;
    
    return 1;
}

/* Destroy a host global area created by host_global_area_create
 * @param hga: Host global area to destroy
 * @param mr: Memory region to unregister */
void host_global_area_destroy(host_global_area_t *hga, struct ibv_mr *mr)
{
    if (mr) {
        host_global_area_unregister_memory(mr);
    }
    if (hga) {
        host_global_area_free_memory(hga);
    }
}
