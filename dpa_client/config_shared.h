#pragma once

#include <stdint.h>

#include "../common/dpa_common.h"
#include "../common/dpastore_common.h"

enum {
    THROUGHPUT_ZIPF_TABLE_MAX_ENTRIES = 1U << 17
};

typedef enum throughput_key_mode {
    THROUGHPUT_KEY_MODE_RANDOM = 0,
    THROUGHPUT_KEY_MODE_UNIFORM = 1,
    THROUGHPUT_KEY_MODE_ZIPF = 2,
} throughput_key_mode;

/*
 * Runtime configuration copied from the host to the DPA throughput client.
 * zipf_table_devaddr   : device pointer to the quantized inverse-CDF table
 * zipf_table_entries   : number of quantiles stored in the table
 * partition_span       : size of each per-thread key range (last thread clamps)
 * partition_count      : number of keys assigned per thread (ceil division)
 * key_min/key_max      : global key bounds for safety clamping
 * seed                 : base seed for per-thread RNGs (host-provided)
 * alpha_q16            : Zipf alpha parameter in Q16 fixed-point (1.1 -> 72090)
 * enable_key_partitioning : non-zero forces per-thread port selection
 */
typedef struct throughput_zipf_config {
    flexio_uintptr_t zipf_table_devaddr;
    uint32_t zipf_table_entries;
    uint64_t partition_span;
    uint64_t partition_count;
    uint64_t key_min;
    uint64_t key_max;
    uint64_t seed;
    uint16_t alpha_q16;
    uint16_t insert_ratio_q16;
    uint8_t enable_key_partitioning;
    uint8_t read_distribution;
    uint8_t insert_distribution;
    uint8_t range_mode;
    uint64_t total_keys;
    /* Dual trace configuration */
    uint8_t main_trace_enabled;
    uint8_t loop_trace_enabled;
    uint8_t main_trace_op; /* DPASTORE_OP_* value */
    uint8_t loop_trace_op; /* DPASTORE_OP_* value */
    uint8_t wrap_main;
    uint8_t trace_reserved0;
    uint16_t main_one_of;
    uint32_t main_trace_keys_per_line;
    uint32_t loop_trace_keys_per_line;
    uint64_t main_trace_total_keys;
    uint64_t loop_trace_total_keys;
    uint64_t main_trace_line_count;
    uint64_t loop_trace_line_count;
    uint64_t main_trace_offset_bytes;
    uint64_t loop_trace_offset_bytes;
} throughput_zipf_config;
