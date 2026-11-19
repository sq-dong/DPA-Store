#ifndef HOST_GLOBAL_AREA_MGMT_H
#define HOST_GLOBAL_AREA_MGMT_H

#include "shared/host_global_area.h"

/* Forward declaration for ibv structures */
struct ibv_pd;
struct ibv_mr;

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize host global area in provided memory
 * Memory must be aligned to 64 bytes (cache line) and registered with ibv_reg_mr
 * @param hga: Pointer to host global area structure
 * @param memory: Aligned memory region for the entire global area
 * @param total_size: Total size of memory region
 * @return: 1 on success, 0 on failure */
int host_global_area_init(
    host_global_area_t *hga,
    void *memory,
    uint64_t total_size);

/* Allocate aligned memory suitable for DPA window access
 * Memory must be 64-byte aligned for window operations
 * @param size: Size of memory to allocate
 * @return: Pointer to aligned memory or NULL on failure */
void* host_global_area_alloc_memory(uint64_t size);

/* Free memory allocated by host_global_area_alloc_memory
 * @param memory: Memory to free */
void host_global_area_free_memory(void *memory);

/* Register memory region for DPA access
 * @param pd: Protection domain
 * @param memory: Memory region to register
 * @param size: Size of memory region
 * @return: Memory region handle or NULL on failure */
struct ibv_mr* host_global_area_register_memory(
    struct ibv_pd *pd,
    void *memory,
    uint64_t size);

/* Unregister memory region
 * @param mr: Memory region to unregister
 * @return: 0 on success, non-zero on failure */
int host_global_area_unregister_memory(struct ibv_mr *mr);

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
    struct ibv_mr **mr_out);

/* Destroy a host global area created by host_global_area_create
 * @param hga: Host global area to destroy
 * @param mr: Memory region to unregister */
void host_global_area_destroy(host_global_area_t *hga, struct ibv_mr *mr);

#ifdef __cplusplus
}
#endif

#endif /* HOST_GLOBAL_AREA_MGMT_H */