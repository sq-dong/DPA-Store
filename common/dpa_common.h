#pragma once

#include "common_cross.h"

#include "dpastore_common.h"
#include "dpastore_common_config.h"

// DPA Store server

#define LOG_SQ_RING_DEPTH 6 /* 2^7 entries, max is 2^15 */
#define LOG_RQ_RING_DEPTH 6 /* 2^7 entries, max is 2^15 */
#define LOG_CQ_RING_DEPTH 6 /* 2^7 entries, max is 2^15 */

#if DPASTORE_RANGE_REQUEST
#define LOG_SQ_DATA_ENTRY_BSIZE 8
#define LOG_RQ_DATA_ENTRY_BSIZE 8
#else
#define LOG_SQ_DATA_ENTRY_BSIZE 7
#define LOG_RQ_DATA_ENTRY_BSIZE 7
#endif


// DPA Store throughput client
#define CLIENT_RING_DEPTH 5
#if DPASTORE_RANGE_REQUEST
#define CLIENT_ENTRY_BSIZE 8
#else
#define CLIENT_ENTRY_BSIZE 7
#endif


struct dpastore_global_init {
    flexio_uintptr_t new_buffer_addr;
    uint32_t new_buffer_mkey_id;
} __attribute__((__packed__, aligned(8)));
