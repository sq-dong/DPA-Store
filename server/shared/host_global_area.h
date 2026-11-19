#ifndef HOST_GLOBAL_AREA_H
#define HOST_GLOBAL_AREA_H

#include <stdint.h>
#include "ringbuffer.h"
#include "commands.h"
#include "server_config.h"

/* Host global area structure that will be accessible from DPA via window
 * Contains embedded ring buffers for communication between DPA and host
 * Memory layout is designed to be cache-line aligned and DMA-friendly
 * All data is at fixed offsets - no pointers needed */
typedef struct {
    /* Patch queues - one per reader thread (DPA to host) */
    /* Each reader thread sends patch commands through its own queue */
    ringbuffer_metadata_t patch_queue_metadata[NUM_PATCH_QUEUES];
    uint8_t patch_queue_buffers[NUM_PATCH_QUEUES][PATCH_QUEUE_SIZE];
    uint8_t padding1[CACHE_LINE_SIZE - ((NUM_PATCH_QUEUES * PATCH_QUEUE_SIZE) % CACHE_LINE_SIZE)];
    
    /* Stitch queues - one per stitcher thread for structural operations (host to DPA) */
    ringbuffer_metadata_t stitch_queue_metadata[NUM_STITCH_QUEUES];
    uint8_t stitch_queue_buffers[NUM_STITCH_QUEUES][STITCH_QUEUE_SIZE];
    uint8_t padding2[CACHE_LINE_SIZE - ((NUM_STITCH_QUEUES * STITCH_QUEUE_SIZE) % CACHE_LINE_SIZE)];
    
    /* Shared state flags */
    volatile uint64_t flags;
    uint8_t padding3[CACHE_LINE_SIZE - sizeof(uint64_t)];
    
    /* Statistics counters */
    volatile uint64_t patches_sent;
    volatile uint64_t patches_received;
    volatile uint64_t stitches_sent;
    volatile uint64_t stitches_received;
    uint8_t padding4[CACHE_LINE_SIZE - 4 * sizeof(uint64_t)];
    
    /* Tree node pools - these are large buffers for allocating tree nodes */
    uint8_t tree_leaf_pool[HOST_TREE_LEAF_POOL_SIZE] __attribute__((aligned(CACHE_LINE_SIZE)));
    uint8_t padding5[CACHE_LINE_SIZE - (HOST_TREE_LEAF_POOL_SIZE % CACHE_LINE_SIZE)];
    
    uint8_t tree_inner_pool[HOST_TREE_INNER_POOL_SIZE] __attribute__((aligned(CACHE_LINE_SIZE)));
    uint8_t padding6[CACHE_LINE_SIZE - (HOST_TREE_INNER_POOL_SIZE % CACHE_LINE_SIZE)];
    
} host_global_area_t;

/* Window configuration for DPA access
 * Stores the memory key and window ID needed for DPA to access host memory */
typedef struct {
    uint32_t window_id;      /* Flex IO window ID */
    uint32_t mkey;           /* Memory key for the registered memory region */
    void *host_memory;       /* Pointer to the host memory region */
    uint64_t memory_size;    /* Total size of the memory region */
    uint64_t dpa_addr;       /* DPA-side address after window mapping */
} host_window_config_t;


/* Host-side functions for receiving commands from DPA */

/* Receive a patch command from a specific reader thread
 * @param hga: Host global area
 * @param thread_id: Reader thread ID (0 to NUM_PATCH_QUEUES-1)
 * @param cmd: Buffer to receive patch command
 * @param max_size: Maximum size of command buffer
 * @param actual_size: Output - actual size received
 * @return: 1 on success, 0 on failure */
static inline int host_receive_patch(host_global_area_t *hga, uint32_t thread_id,
                                     void *cmd, uint32_t max_size, uint32_t *actual_size)
{
    if (ringbuffer_consume(&hga->patch_queue_metadata[thread_id],
                          hga->patch_queue_buffers[thread_id],
                          PATCH_QUEUE_SIZE, cmd, max_size, actual_size)) {
        return 1;
    }
    return 0;
}

/* Send a stitch command from host to DPA
 * @param hga: Host global area
 * @param cmd: Stitch command to send
 * @param size: Size of the command
 * @return: 1 on success, 0 on failure */
static inline int host_send_stitch(host_global_area_t *hga, uint32_t queue_id, const void *cmd, uint32_t size)
{
#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (!hga) {
        return 0;
    }

    if (NUM_STITCH_QUEUES == 0) {
        return 0;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER

    uint32_t queue = queue_id % NUM_STITCH_QUEUES;

    while (1) {
        if (ringbuffer_produce(&hga->stitch_queue_metadata[queue], hga->stitch_queue_buffers[queue],
                          STITCH_QUEUE_SIZE, cmd, size)) {
            return 1; 
        }
        // } else {
        //     printf("WAITING\n");
        // }

            
    } 
    
}

/* DPA-side functions (to be called from DPA code) */
#ifdef __DEVICE__

/* Acquire an in-place slot in the patch queue for the given thread. The caller
 * must populate the returned payload and follow up with dpa_patch_commit
 * before attempting another acquire. Returns 1 on success, 0 if the queue is
 * full. */
static inline int dpa_patch_acquire(host_global_area_t *hga, uint32_t thread_id,
                                    uint32_t size, void **payload_ptr)
{
    return ringbuffer_produce_acquire(&hga->patch_queue_metadata[thread_id],
                                      hga->patch_queue_buffers[thread_id],
                                      PATCH_QUEUE_SIZE, size, payload_ptr);
}

/* Commit a previously acquired patch payload to make it visible to the host. */
static inline void dpa_patch_commit(host_global_area_t *hga, uint32_t thread_id,
                                    void *payload_ptr, uint32_t size)
{
    ringbuffer_produce_commit(&hga->patch_queue_metadata[thread_id],
                              hga->patch_queue_buffers[thread_id],
                              PATCH_QUEUE_SIZE, payload_ptr, size);
}

/* Send a patch command from DPA reader thread to host
 * @param hga: Host global area (accessed via window)
 * @param thread_id: Reader thread ID (0 to NUM_PATCH_QUEUES-1)
 * @param cmd: Patch command to send
 * @param size: Size of the patch command
 * @return: 1 on success, 0 on failure */
static inline int dpa_send_patch(host_global_area_t *hga, uint32_t thread_id,
                                 const void *cmd, uint32_t size)
{
    void *slot_ptr;
    if (!ringbuffer_produce_acquire(&hga->patch_queue_metadata[thread_id],
                                    hga->patch_queue_buffers[thread_id],
                                    PATCH_QUEUE_SIZE, size, &slot_ptr)) {
        return 0;
    }

    ringbuffer_memcpy(slot_ptr, cmd, size);
    ringbuffer_produce_commit(&hga->patch_queue_metadata[thread_id],
                              hga->patch_queue_buffers[thread_id],
                              PATCH_QUEUE_SIZE, slot_ptr, size);
    return 1;
}

/* DPA will receive stitch commands from host - to be implemented later */

#endif /* __DEVICE__ */


#endif /* HOST_GLOBAL_AREA_H */
