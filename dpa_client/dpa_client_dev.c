#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <libflexio-dev/flexio_dev_ver.h>
#define FLEXIO_DEV_VER_USED FLEXIO_DEV_VER(25, 10, 0)

#include <wrapper_flexio_device.h>
#include "../common/dpa_common.h"
#include "../common/dpastore_common_config.h"
#include "../common/dpastore_common.h"
#include "../common/uniform_catalog.h"
#include "config_shared.h"

typedef DPAStoreClientPacket DPAStorePacket;

// --- Simple packet field accessors ---
#define PKT_UDP_DST_PORT(data) ((uint16_t*)((data) + 14 + 20 + 2))

// --- Type definitions ---

#define PACKET_SIZE (sizeof(DPAStorePacket))

typedef struct dev_ctx_worker
{
    uint32_t lkey;
    uint32_t is_initalized;
    struct cq_ctx_t rqcq_ctx;
    struct cq_ctx_t sqcq_ctx;
    struct rq_ctx_t rq_ctx;
    struct sq_ctx_t sq_ctx;
    struct dt_ctx_t dt_ctx;
    struct host_rq_ctx_t host_rq_ctx;
    uint32_t packets_count;
#if DPASTORE_PERFORMANCE_COUNTING
    // Periodic throughput measurement (every 2 seconds)
    uint64_t periodic_count;         // Packet count for current 2-second window
    uint64_t periodic_total_count;   // Total packets since start for average calculation
    uint64_t waiter;
#endif
    uint8_t rq_on_host;
    uint8_t sq_on_host;
    uint32_t thread_index;
    char packet_template[PACKET_SIZE];
    uint64_t rng_state;
    uint64_t key_range_base;
    uint64_t key_range_limit;
    uint64_t key_range_span;
    uint64_t current_key;  // Current key for cycling through range
    uint32_t current_port_offset;  // Current port offset for cycling through UDP ports
    uint32_t port_stride;          // Step size used when rotating across server ports
    uint32_t trace_thread_id;
    const uint64_t *main_trace_ptr;
    const uint64_t *loop_trace_ptr;
    uint32_t main_one_of_counter;
    uint8_t stop_sending;
} dev_ctx_worker;


// --- Global variables ---

struct dev_ctx_worker dev_ctx_readers[DPASTORE_READER_THREAD_COUNT] = {0};

typedef struct throughput_runtime_state {
    const uint32_t *zipf_table;
    uint32_t table_entries;
    uint64_t partition_span;
    uint64_t partition_count;
    uint64_t total_keys;
    uint64_t key_min;
   uint64_t key_max;
   uint64_t seed;
    uint8_t enable_key_partitioning;
    uint8_t read_distribution;
    uint8_t insert_distribution;
    uint32_t insert_threshold;
    uint8_t range_mode;
    /* Trace replay configuration */
    uint64_t main_trace_total_keys;
    uint64_t main_trace_line_count;
    uint64_t loop_trace_total_keys;
    uint64_t loop_trace_line_count;
    uint32_t main_trace_keys_per_line;
    uint32_t loop_trace_keys_per_line;
    uint8_t main_trace_enabled;
    uint8_t loop_trace_enabled;
    uint8_t main_trace_op;
    uint8_t loop_trace_op;
    uint8_t wrap_main;
    uint16_t main_one_of;
    uint64_t main_trace_offset_bytes;
    uint64_t loop_trace_offset_bytes;
} throughput_runtime_state;

static throughput_runtime_state g_runtime_state = {
    .zipf_table = NULL,
    .table_entries = 0,
    .partition_span = 1,
    .partition_count = 1,
    .total_keys = DPASTORE_BULK_LOAD_NUM_KEYS,
    .key_min = DPASTORE_BULK_LOAD_MIN_KEY,
    .key_max = DPASTORE_BULK_LOAD_MAX_KEY,
    .seed = 0x5851f42d4c957f2dULL,
    .enable_key_partitioning = 1,
    .read_distribution = THROUGHPUT_KEY_MODE_ZIPF,
    .insert_distribution = THROUGHPUT_KEY_MODE_ZIPF,
    .insert_threshold = 0,
    .range_mode = 0,
    .main_trace_total_keys = 0,
    .main_trace_line_count = 0,
    .loop_trace_total_keys = 0,
    .loop_trace_line_count = 0,
    .main_trace_keys_per_line = 0,
    .loop_trace_keys_per_line = 0,
    .main_trace_enabled = 0,
    .loop_trace_enabled = 0,
    .main_trace_op = DPASTORE_OP_GET,
    .loop_trace_op = DPASTORE_OP_GET,
    .wrap_main = 1,
    .main_one_of = 1,
    .main_trace_offset_bytes = 0,
    .loop_trace_offset_bytes = 0
};

static UniformPlan g_partition_plans[DPASTORE_READER_THREAD_COUNT];
static uint8_t g_partition_plan_ready = 0;

typedef struct trace_cursor_state {
    uint64_t line;
    uint32_t offset;
    uint32_t padding;
    uint64_t padding2[6];
} trace_cursor_state_t;

static trace_cursor_state_t g_trace_main_cursors[DPASTORE_READER_THREAD_COUNT];
static trace_cursor_state_t g_trace_loop_cursors[DPASTORE_READER_THREAD_COUNT];
static flexio_uintptr_t g_trace_host_addr = 0;
static uint32_t g_trace_window_id = 0;
static uint32_t g_trace_mkey = 0;
static uint64_t g_trace_main_offset_bytes = 0;
static uint64_t g_trace_loop_offset_bytes = 0;

/* Forward decl for trace helper */
static inline bool trace_mode_enabled(void);

static inline void ensure_trace_mapping(dev_ctx_worker *ctx)
{
    if (!trace_mode_enabled()) {
        return;
    }
    if ((g_runtime_state.main_trace_enabled /*&& ctx->main_trace_ptr == NULL*/) ||
        (g_runtime_state.loop_trace_enabled /*&& ctx->loop_trace_ptr == NULL*/)) {
        if (g_trace_host_addr == 0 || g_trace_window_id == 0 || g_trace_mkey == 0) {
            return;
        }
        flexio_dev_window_config(FLEXIO_DEV_WINDOW_ENTITY_0, g_trace_window_id, g_trace_mkey);
        flexio_uintptr_t dpa_addr = 0;
        flexio_dev_status_t status = flexio_dev_window_ptr_acquire(FLEXIO_DEV_WINDOW_ENTITY_0,
                                                                   g_trace_host_addr,
                                                                   &dpa_addr);
        if (status != FLEXIO_DEV_STATUS_SUCCESS) {
            return;
        }
        if (g_runtime_state.main_trace_enabled && ctx->main_trace_ptr == NULL) {
            ctx->main_trace_ptr = (const uint64_t *)(dpa_addr + g_trace_main_offset_bytes);
        }
        if (g_runtime_state.loop_trace_enabled && ctx->loop_trace_ptr == NULL) {
            ctx->loop_trace_ptr = (const uint64_t *)(dpa_addr + g_trace_loop_offset_bytes);
        }
    }
}

static inline uint64_t saturating_add_u64(uint64_t a, uint64_t b)
{
    uint64_t result = a + b;
    if (result < a) {
        return UINT64_MAX;
    }
    return result;
}

static inline uint64_t saturating_mul_u64(uint64_t a, uint64_t b)
{
#if defined(__SIZEOF_INT128__)
    __uint128_t prod = (__uint128_t)a * (__uint128_t)b;
    if (prod > ( __uint128_t )UINT64_MAX) {
        return UINT64_MAX;
    }
    return (uint64_t)prod;
#else
    if (a == 0ULL || b == 0ULL) {
        return 0ULL;
    }
    if (a > UINT64_MAX / b) {
        return UINT64_MAX;
    }
    return a * b;
#endif
}

static void rebuild_partition_plans(void)
{
    uint64_t key_max_plus_one = saturating_add_u64(g_runtime_state.key_max, 1ULL);

    for (uint32_t bucket = 0; bucket < DPASTORE_READER_THREAD_COUNT; ++bucket) {
        uint64_t offset = saturating_mul_u64((uint64_t)bucket, g_runtime_state.partition_span);
        uint64_t base = saturating_add_u64(g_runtime_state.key_min, offset);

        uint64_t limit = saturating_add_u64(base, g_runtime_state.partition_span);
        if (key_max_plus_one != UINT64_MAX && limit > key_max_plus_one) {
            limit = key_max_plus_one;
        }
        if (limit <= base) {
            limit = saturating_add_u64(base, 1ULL);
        }
        uint64_t span = (limit > base) ? (limit - base) : 1ULL;

        uint64_t partition_count = g_runtime_state.partition_count;
        uint64_t start_index = saturating_mul_u64((uint64_t)bucket, partition_count);
        uint64_t remaining = (g_runtime_state.total_keys > start_index)
            ? (g_runtime_state.total_keys - start_index)
            : 0ULL;
        uint64_t count = partition_count;
        if (remaining < partition_count) {
            count = remaining;
        }
        if (count == 0ULL) {
            count = 1ULL;
        }
        if (count > span) {
            count = span;
        }

        uint64_t plan_seed = g_runtime_state.seed ^ (0x9e3779b97f4a7c15ULL * ((uint64_t)bucket + 1ULL));
        if (plan_seed == 0ULL) {
            plan_seed = 0x5851f42d4c957f2dULL ^ ((uint64_t)bucket + 1ULL);
        }
        uniform_plan_init(&g_partition_plans[bucket], base, span, count, plan_seed);
    }

    g_partition_plan_ready = 1;
}

static inline const UniformPlan *get_partition_plan(uint32_t bucket)
{
    if (!g_partition_plan_ready) {
        rebuild_partition_plans();
    }
    bucket %= DPASTORE_READER_THREAD_COUNT;
    return &g_partition_plans[bucket];
}

__dpa_rpc__ uint64_t dpastore_device_set_trace_windows(uint64_t data)
{
    struct {
        flexio_uintptr_t base_host_addr;
        uint32_t window_id;
        uint32_t mkey;
        uint64_t main_offset_bytes;
        uint64_t loop_offset_bytes;
    } *window_info = (void *)data;

    g_trace_host_addr = window_info->base_host_addr;
    g_trace_window_id = window_info->window_id;
    g_trace_mkey = window_info->mkey;
    g_trace_main_offset_bytes = window_info->main_offset_bytes;
    g_trace_loop_offset_bytes = window_info->loop_offset_bytes;

    return 0;
}

__dpa_rpc__ uint64_t dpastore_device_apply_throughput_config(uint64_t data)
{
    throughput_zipf_config *cfg = (throughput_zipf_config *)data;
    g_runtime_state.zipf_table = (const uint32_t *)(uintptr_t)cfg->zipf_table_devaddr;
    g_runtime_state.table_entries = cfg->zipf_table_entries;
    g_runtime_state.partition_span = cfg->partition_span ? cfg->partition_span : 1ULL;
    g_runtime_state.partition_count = cfg->partition_count ? cfg->partition_count : 1ULL;
    g_runtime_state.total_keys = cfg->total_keys ? cfg->total_keys : DPASTORE_BULK_LOAD_NUM_KEYS;
    g_runtime_state.key_min = cfg->key_min;
    g_runtime_state.key_max = cfg->key_max;
    g_runtime_state.seed = cfg->seed ? cfg->seed : 0x5851f42d4c957f2dULL;
    g_runtime_state.enable_key_partitioning = cfg->enable_key_partitioning;
    g_runtime_state.read_distribution = cfg->read_distribution;
    g_runtime_state.insert_distribution = cfg->insert_distribution;
    uint32_t insert_threshold = cfg->insert_ratio_q16;
    if (insert_threshold > 65535U) {
        insert_threshold = 65535U;
    }
    g_runtime_state.insert_threshold = insert_threshold;
    g_runtime_state.range_mode = cfg->range_mode;
    g_runtime_state.main_trace_total_keys = cfg->main_trace_total_keys;
    g_runtime_state.main_trace_line_count = cfg->main_trace_line_count;
    g_runtime_state.main_trace_keys_per_line = cfg->main_trace_keys_per_line;
    g_runtime_state.loop_trace_total_keys = cfg->loop_trace_total_keys;
    g_runtime_state.loop_trace_line_count = cfg->loop_trace_line_count;
    g_runtime_state.loop_trace_keys_per_line = cfg->loop_trace_keys_per_line;
    g_runtime_state.main_trace_op = cfg->main_trace_op;
    g_runtime_state.loop_trace_op = cfg->loop_trace_op;
    g_runtime_state.wrap_main = cfg->wrap_main;
    g_runtime_state.main_one_of = cfg->main_one_of ? cfg->main_one_of : 1;

    g_runtime_state.main_trace_offset_bytes = cfg->main_trace_offset_bytes;
    g_runtime_state.loop_trace_offset_bytes = cfg->loop_trace_offset_bytes;

    g_runtime_state.main_trace_enabled = (cfg->main_trace_enabled &&
        g_trace_host_addr != 0 && g_trace_window_id != 0 && g_trace_mkey != 0 &&
        g_runtime_state.main_trace_keys_per_line > 0 && g_runtime_state.main_trace_line_count > 0) ? 1 : 0;
    g_runtime_state.loop_trace_enabled = (cfg->loop_trace_enabled &&
        g_trace_host_addr != 0 && g_trace_window_id != 0 && g_trace_mkey != 0 &&
        g_runtime_state.loop_trace_keys_per_line > 0 && g_runtime_state.loop_trace_line_count > 0) ? 1 : 0;

    for (uint32_t i = 0; i < DPASTORE_READER_THREAD_COUNT; ++i) {
        g_trace_main_cursors[i].line = g_runtime_state.main_trace_line_count
            ? (uint64_t)(i % g_runtime_state.main_trace_line_count)
            : 0ULL;
        g_trace_main_cursors[i].offset = 0;
        g_trace_loop_cursors[i].line = g_runtime_state.loop_trace_line_count
            ? (uint64_t)(i % g_runtime_state.loop_trace_line_count)
            : 0ULL;
        g_trace_loop_cursors[i].offset = 0;
    }

    g_partition_plan_ready = 0;
    rebuild_partition_plans();
    flexio_dev_print("Throughput config: entries=%u span=%llu count=%llu seed=%#llx partitioning=%u\n",
                     g_runtime_state.table_entries,
                     (unsigned long long)g_runtime_state.partition_span,
                     (unsigned long long)g_runtime_state.partition_count,
                     (unsigned long long)g_runtime_state.seed,
                     g_runtime_state.enable_key_partitioning);
    flexio_dev_print("Throughput mix: insert_threshold=%u read_mode=%u insert_mode=%u\n",
                     (unsigned)g_runtime_state.insert_threshold,
                     (unsigned)g_runtime_state.read_distribution,
                     (unsigned)g_runtime_state.insert_distribution);
    return 0;
}


// --- NIC Counter Reporting (like DPA store) ---
typedef struct throughput_client_reporter {
    uint32_t counter_ids[8];
    uint32_t counter_values[8];
} throughput_client_reporter_t;

static throughput_client_reporter_t g_reporter = {0};

static inline uint64_t prng_next(dev_ctx_worker *ctx)
{
    uint64_t x = ctx->rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    if (x == 0ULL) {
        x = 0x9e3779b97f4a7c15ULL;
    }
    ctx->rng_state = x;
    return x;
}

static inline uint64_t zipf_rank_for_plan(dev_ctx_worker *ctx, const UniformPlan *plan)
{
    uint64_t count = (plan->count != 0ULL) ? plan->count : 1ULL;

    if (g_runtime_state.zipf_table == NULL || g_runtime_state.table_entries == 0U) {
        return prng_next(ctx) % count;
    }

    uint64_t rnd = prng_next(ctx);
    uint32_t index = (uint32_t)((rnd >> 48) % g_runtime_state.table_entries);
    uint64_t rank = g_runtime_state.zipf_table[index];
    if (rank >= count) {
        rank = rnd % count;
    }
    return rank;
}

static inline uint64_t generate_zipf_key_for_plan(dev_ctx_worker *ctx, const UniformPlan *plan)
{
    uint64_t rank = zipf_rank_for_plan(ctx, plan);
    uint64_t key = uniform_key_for_rank(plan, rank);
    if (key < g_runtime_state.key_min) {
        key = g_runtime_state.key_min;
    }
    if (key > g_runtime_state.key_max) {
        key = g_runtime_state.key_max;
    }
    return key;
}

static inline uint64_t generate_uniform_key_for_plan(dev_ctx_worker *ctx, const UniformPlan *plan)
{
    uint64_t count = (plan->count != 0ULL) ? plan->count : 1ULL;
    uint64_t rank = count ? (prng_next(ctx) % count) : 0ULL;
    uint64_t key = uniform_key_for_rank(plan, rank);
    if (key < g_runtime_state.key_min) {
        key = g_runtime_state.key_min;
    }
    if (key > g_runtime_state.key_max) {
        key = g_runtime_state.key_max;
    }
    return key;
}

static inline uint64_t generate_global_random_key(dev_ctx_worker *ctx)
{
    uint64_t max_plus_one = saturating_add_u64(g_runtime_state.key_max, 1ULL);
    uint64_t range = (max_plus_one > g_runtime_state.key_min)
        ? (max_plus_one - g_runtime_state.key_min)
        : 1ULL;
    if (range == 0ULL) {
        range = 1ULL;
    }
    uint64_t rnd = prng_next(ctx);
    uint64_t offset = range ? (rnd % range) : 0ULL;
    uint64_t key = g_runtime_state.key_min + offset;
    if (key > g_runtime_state.key_max) {
        key = g_runtime_state.key_max;
    }
    return key;
}

static inline uint64_t generate_key_for_mode(dev_ctx_worker *ctx,
                                             const UniformPlan *plan,
                                             uint8_t mode)
{
    switch (mode) {
        case THROUGHPUT_KEY_MODE_RANDOM:
            return generate_global_random_key(ctx);
        case THROUGHPUT_KEY_MODE_UNIFORM:
            return generate_uniform_key_for_plan(ctx, plan);
        case THROUGHPUT_KEY_MODE_ZIPF:
        default:
            return generate_zipf_key_for_plan(ctx, plan);
    }
}

static inline bool trace_mode_enabled(void)
{
    return g_runtime_state.main_trace_enabled || g_runtime_state.loop_trace_enabled;
}

static inline uint64_t trace_next_key(dev_ctx_worker *ctx,
                                      bool use_main,
                                      trace_cursor_state_t *cursor,
                                      const uint64_t *trace_ptr,
                                      uint64_t line_count,
                                      uint32_t keys_per_line,
                                      bool wrap_allowed,
                                      uint8_t *should_stop)
{
    if (trace_ptr == NULL || line_count == 0 || keys_per_line == 0) {
        return 0ULL;
    }

    uint64_t line = cursor->line;
    uint32_t offset = cursor->offset;

    if (line >= line_count) {
        line %= line_count;
    }
    if (offset >= keys_per_line) {
        offset = 0;
    }

    const uint64_t *line_base = trace_ptr + (line * keys_per_line);
    uint64_t key = line_base[offset];

    offset++;
    if (offset >= keys_per_line) {
        offset = 0;
        line += DPASTORE_READER_THREAD_COUNT;
        if (line >= line_count) {
            if (!wrap_allowed && use_main) {
                *should_stop = 1;
                cursor->line = line_count;
                cursor->offset = keys_per_line;
                return key;
            }
            line %= line_count;
        }
    }

    cursor->line = line;
    cursor->offset = offset;
    return key;
}

static inline uint32_t pick_partition_bucket(dev_ctx_worker *ctx)
{
    if (!g_runtime_state.enable_key_partitioning) {
        uint32_t rnd = (uint32_t)(prng_next(ctx) & 0xFFFFFFFFU);
        return rnd % DPASTORE_READER_THREAD_COUNT;
    }

    uint32_t bucket = ctx->current_port_offset;
    uint32_t next = bucket + 1;
    if (next >= DPASTORE_READER_THREAD_COUNT) {
        next %= DPASTORE_READER_THREAD_COUNT;
    }
    ctx->current_port_offset = next;
    return bucket;
}

// Initialize NIC counters configuration
static void init_nic_counters(void) {
    g_reporter.counter_ids[0] = FLEXIO_DEV_NIC_COUNTER_PORT0_RX_BYTES;
    g_reporter.counter_ids[1] = FLEXIO_DEV_NIC_COUNTER_PORT1_RX_BYTES;
    g_reporter.counter_ids[2] = FLEXIO_DEV_NIC_COUNTER_PORT2_RX_BYTES;
    g_reporter.counter_ids[3] = FLEXIO_DEV_NIC_COUNTER_PORT3_RX_BYTES;
    g_reporter.counter_ids[4] = FLEXIO_DEV_NIC_COUNTER_PORT0_TX_BYTES;
    g_reporter.counter_ids[5] = FLEXIO_DEV_NIC_COUNTER_PORT1_TX_BYTES;
    g_reporter.counter_ids[6] = FLEXIO_DEV_NIC_COUNTER_PORT2_TX_BYTES;
    g_reporter.counter_ids[7] = FLEXIO_DEV_NIC_COUNTER_PORT3_TX_BYTES;

    flexio_dev_nic_counters_config(g_reporter.counter_values, g_reporter.counter_ids, 8);
}

// Print NIC counter statistics
static void print_nic_counters(void) {
    g_reporter.counter_ids[0] = FLEXIO_DEV_NIC_COUNTER_PORT0_RX_BYTES;
    g_reporter.counter_ids[1] = FLEXIO_DEV_NIC_COUNTER_PORT1_RX_BYTES;
    g_reporter.counter_ids[2] = FLEXIO_DEV_NIC_COUNTER_PORT2_RX_BYTES;
    g_reporter.counter_ids[3] = FLEXIO_DEV_NIC_COUNTER_PORT3_RX_BYTES;
    g_reporter.counter_ids[4] = FLEXIO_DEV_NIC_COUNTER_PORT0_TX_BYTES;
    g_reporter.counter_ids[5] = FLEXIO_DEV_NIC_COUNTER_PORT1_TX_BYTES;
    g_reporter.counter_ids[6] = FLEXIO_DEV_NIC_COUNTER_PORT2_TX_BYTES;
    g_reporter.counter_ids[7] = FLEXIO_DEV_NIC_COUNTER_PORT3_TX_BYTES;

    flexio_dev_nic_counters_config(g_reporter.counter_values, g_reporter.counter_ids, 8);

    flexio_dev_nic_counters_sample();
    flexio_dev_print("DPA Throughput Client Counters: RX[%u,%u,%u,%u] TX[%u,%u,%u,%u]\n",
                     g_reporter.counter_values[0], g_reporter.counter_values[1],
                     g_reporter.counter_values[2], g_reporter.counter_values[3],
                     g_reporter.counter_values[4], g_reporter.counter_values[5],
                     g_reporter.counter_values[6], g_reporter.counter_values[7]);
}

#if DPASTORE_PERFORMANCE_COUNTING
// Function to print and reset periodic throughput stats
void __dpa_global__ print_and_reset_periodic_throughput() {
    flexio_dev_print("DPA: === 2-Second Throughput Stats ===\n");

    uint64_t total_window_packets = 0;
    uint64_t total_cumulative_packets = 0;

    for (int i = 0; i < DPASTORE_READER_THREAD_COUNT; i++) {
        struct dev_ctx_worker* ctx = &dev_ctx_readers[i];
        if (ctx->periodic_total_count > 0) {
            // flexio_dev_print("DPA: Thread %d: %lu pps (2s window: %lu packets, total: %lu)\n",
            //      i, ctx->periodic_count / 2, ctx->periodic_count, ctx->periodic_total_count);

            total_window_packets += ctx->periodic_count;
            total_cumulative_packets += ctx->periodic_total_count;

            // Reset window counter (keep total counter)
            ctx->periodic_count = 0;
        }
    }

    if (total_window_packets > 0) {
        uint64_t pps = total_window_packets / 2;
        uint64_t mops = pps / 1000000;  // Mega operations per second
        uint64_t mops_frac = (pps % 1000000) / 1000;  // Fractional part in thousands
        flexio_dev_print("DPA: Total: %lu pps (%lu.%03lu Mops) (2s window: %lu packets, cumulative: %lu)\n",
            pps, mops, mops_frac, total_window_packets, total_cumulative_packets);
    }

    print_nic_counters();

    flexio_dev_print("DPA: === End 2s Stats ===\n");
}
#endif // DPASTORE_PERFORMANCE_COUNTING



// --- CQ, RQ, SQ initialization ---

static void init_cq(const struct app_transfer_cq app_cq, struct cq_ctx_t *ctx)
{
    ctx->cq_number = app_cq.cq_num;
    ctx->cq_ring = (struct flexio_dev_cqe64 *)app_cq.cq_ring_daddr;
    ctx->cq_dbr = (uint32_t *)app_cq.cq_dbr_daddr;
    ctx->cqe = ctx->cq_ring;
    ctx->cq_idx = 0;
    ctx->cq_hw_owner_bit = 0x1;
    ctx->cq_idx_mask = ((1 << CLIENT_RING_DEPTH) - 1);
}

static void init_rq(const struct app_transfer_wq app_rq, struct rq_ctx_t *ctx)
{
    ctx->rq_number = app_rq.wq_num;
    ctx->rq_ring = (struct flexio_dev_wqe_rcv_data_seg *)app_rq.wq_ring_daddr;
    ctx->rq_dbr = (uint32_t *)app_rq.wq_dbr_daddr;
    ctx->rq_idx_mask = ((1 << CLIENT_RING_DEPTH) - 1);
}

static void init_sq(const struct app_transfer_wq app_sq, struct sq_ctx_t *ctx)
{
    ctx->sq_number = app_sq.wq_num;
    ctx->sq_ring = (union flexio_dev_sqe_seg *)app_sq.wq_ring_daddr;
    ctx->sq_dbr = (uint32_t *)app_sq.wq_dbr_daddr;
    ctx->sq_wqe_seg_idx = 0;
    ctx->sq_dbr++;
    ctx->sq_idx_mask = ((1 << (CLIENT_RING_DEPTH + LOG_SQE_NUM_SEGS)) - 1);
}

// --- Device initialization ---

__dpa_rpc__ uint64_t dpastore_device_init_global(uint64_t data)
{
    struct dpastore_global_init *shared_data = (struct dpastore_global_init *)data;
    LOG_I("DPA: device global initialized\n");
    return 0;
}

// --- Worker (multi-threaded) initialization and event handling ---

static uint64_t dpastore_device_init_worker(struct dev_ctx_worker* _dev_ctx, struct queue_config_data *shared_data)
{
    _dev_ctx->lkey = shared_data->sq_data.wqd_mkey_id;
    _dev_ctx->host_rq_ctx.rkey = shared_data->rq_data.wqd_mkey_id;
    _dev_ctx->host_rq_ctx.rq_window_id = shared_data->new_buffer_mkey_id;
    _dev_ctx->host_rq_ctx.host_rx_buff = (void *)shared_data->rq_data.wqd_daddr;

    init_cq(shared_data->rq_cq_data, &_dev_ctx->rqcq_ctx);
    init_rq(shared_data->rq_data, &_dev_ctx->rq_ctx);
    init_cq(shared_data->sq_cq_data, &_dev_ctx->sqcq_ctx);
    init_sq(shared_data->sq_data, &_dev_ctx->sq_ctx);

    init_send_sq(&_dev_ctx->sq_ctx, LOG2VALUE(CLIENT_RING_DEPTH), _dev_ctx->lkey);
    _dev_ctx->dt_ctx.sq_tx_buff = (void *)shared_data->sq_data.wqd_daddr;
    _dev_ctx->dt_ctx.tx_buff_idx = 0;
    _dev_ctx->dt_ctx.data_idx_mask = ((1 << (CLIENT_RING_DEPTH)) - 1);

    _dev_ctx->rq_on_host = shared_data->rq_data.daddr_on_host;
    _dev_ctx->sq_on_host = shared_data->sq_data.daddr_on_host;

    _dev_ctx->is_initalized = 1;
    return 0;
}

__dpa_rpc__ uint64_t dpastore_device_init_dpastore_reader(uint64_t data)
{
    struct queue_config_data *shared_data = (struct queue_config_data *)data;
    uint32_t thread_index = shared_data->thread_index;
    struct dev_ctx_worker* _dev_ctx = &dev_ctx_readers[thread_index];
    _dev_ctx->thread_index = thread_index;

    uint64_t rng_seed = g_runtime_state.seed ^ (0x9e3779b97f4a7c15ULL * (thread_index + 1ULL));
    if (rng_seed == 0ULL) {
        rng_seed = 0x9e3779b97f4a7c15ULL ^ ((uint64_t)thread_index + 1ULL);
    }
    _dev_ctx->rng_state = rng_seed;
    _dev_ctx->current_port_offset = thread_index % DPASTORE_READER_THREAD_COUNT;

    const UniformPlan *start_plan = get_partition_plan(_dev_ctx->current_port_offset);
    _dev_ctx->key_range_base = start_plan->base;
    _dev_ctx->key_range_span = start_plan->span ? start_plan->span : 1ULL;
    _dev_ctx->key_range_limit = saturating_add_u64(_dev_ctx->key_range_base, _dev_ctx->key_range_span);
    _dev_ctx->current_key = _dev_ctx->key_range_base;

    uint64_t stride_seed = rng_seed ^ 0x8badf00dULL;
    uint64_t stride64 = uniform_pick_stride(DPASTORE_READER_THREAD_COUNT, stride_seed);
    uint32_t stride = (uint32_t)(stride64 % DPASTORE_READER_THREAD_COUNT);
    if (stride == 0U) {
        stride = 1U;
    }
    _dev_ctx->port_stride = stride;

    /* Map trace buffers per thread if enabled */
    _dev_ctx->main_trace_ptr = NULL;
    _dev_ctx->loop_trace_ptr = NULL;
    _dev_ctx->trace_thread_id = thread_index;
    _dev_ctx->main_one_of_counter = 0;
    _dev_ctx->stop_sending = 0;

    if (g_runtime_state.main_trace_enabled &&
        g_trace_host_addr != 0 &&
        g_trace_window_id != 0 &&
        g_trace_mkey != 0) {
        flexio_dev_window_config(FLEXIO_DEV_WINDOW_ENTITY_0, g_trace_window_id, g_trace_mkey);
        flexio_uintptr_t dpa_addr = 0;
        flexio_dev_status_t status = flexio_dev_window_ptr_acquire(FLEXIO_DEV_WINDOW_ENTITY_0,
                                                                   g_trace_host_addr,
                                                                   &dpa_addr);
        if (status == FLEXIO_DEV_STATUS_SUCCESS) {
            _dev_ctx->main_trace_ptr = (const uint64_t *)(dpa_addr + g_trace_main_offset_bytes);
            flexio_dev_print("DPA main-trace map ok: thread %u ptr=%p line_count=%llu keys_per_line=%u\n",
                             (unsigned)thread_index,
                             (void *)_dev_ctx->main_trace_ptr,
                             (unsigned long long)g_runtime_state.main_trace_line_count,
                             (unsigned)g_runtime_state.main_trace_keys_per_line);
        } else {
            flexio_dev_print("DPA main-trace map failed: thread %u status=%d\n",
                             (unsigned)thread_index, status);
        }
    }
    if (g_runtime_state.loop_trace_enabled &&
        g_trace_host_addr != 0 &&
        g_trace_window_id != 0 &&
        g_trace_mkey != 0) {
        flexio_dev_window_config(FLEXIO_DEV_WINDOW_ENTITY_0, g_trace_window_id, g_trace_mkey);
        flexio_uintptr_t dpa_addr = 0;
        flexio_dev_status_t status = flexio_dev_window_ptr_acquire(FLEXIO_DEV_WINDOW_ENTITY_0,
                                                                   g_trace_host_addr,
                                                                   &dpa_addr);
        if (status == FLEXIO_DEV_STATUS_SUCCESS) {
            _dev_ctx->loop_trace_ptr = (const uint64_t *)(dpa_addr + g_trace_loop_offset_bytes);
            flexio_dev_print("DPA loop-trace map ok: thread %u ptr=%p line_count=%llu keys_per_line=%u\n",
                             (unsigned)thread_index,
                             (void *)_dev_ctx->loop_trace_ptr,
                             (unsigned long long)g_runtime_state.loop_trace_line_count,
                             (unsigned)g_runtime_state.loop_trace_keys_per_line);
        } else {
            flexio_dev_print("DPA loop-trace map failed: thread %u status=%d\n",
                             (unsigned)thread_index, status);
        }
    }

    uint64_t ret = dpastore_device_init_worker(_dev_ctx, shared_data);
    flexio_dev_print("DPA reader: device init, thread %u rotating across %u ports (start bucket %u, key base %llu, stride %u)\n",
                     thread_index,
                     DPASTORE_READER_THREAD_COUNT,
                     _dev_ctx->current_port_offset,
                     (unsigned long long)_dev_ctx->key_range_base,
                     _dev_ctx->port_stride);
    if (trace_mode_enabled() && g_runtime_state.main_trace_line_count > 0) {
        g_trace_main_cursors[thread_index % DPASTORE_READER_THREAD_COUNT].line =
            (uint64_t)(thread_index % g_runtime_state.main_trace_line_count);
        g_trace_main_cursors[thread_index % DPASTORE_READER_THREAD_COUNT].offset = 0;
    }
    if (trace_mode_enabled() && g_runtime_state.loop_trace_line_count > 0) {
        g_trace_loop_cursors[thread_index % DPASTORE_READER_THREAD_COUNT].line =
            (uint64_t)(thread_index % g_runtime_state.loop_trace_line_count);
        g_trace_loop_cursors[thread_index % DPASTORE_READER_THREAD_COUNT].offset = 0;
    }
    return ret;
}


static inline void dpastore_device_event_handler_worker(
    struct dev_ctx_worker* _dev_ctx,
    void (*process_packet_fn)(struct dev_ctx_worker*, struct flexio_dev_thread_ctx*),
    const char* log_prefix,
    uint64_t index)
{
    struct flexio_dev_thread_ctx *dtctx;
    flexio_dev_get_thread_ctx(&dtctx);

    if (_dev_ctx->is_initalized == 0)
        flexio_dev_thread_reschedule();

    _dev_ctx->host_rq_ctx.dpa_rx_buff = (flexio_uintptr_t)_dev_ctx->host_rq_ctx.host_rx_buff;

    ensure_trace_mapping(_dev_ctx);

    LOG_I("%s: event begin, thread %lu\n", log_prefix, index);
    while (dtctx != NULL)
    {
        while (flexio_dev_cqe_get_owner(_dev_ctx->rqcq_ctx.cqe) != _dev_ctx->rqcq_ctx.cq_hw_owner_bit)
        {
            process_packet_fn(_dev_ctx, dtctx);
            step_rq(&_dev_ctx->rq_ctx);
            step_cq(&_dev_ctx->rqcq_ctx);
        }
    }

    flexio_dev_print("%s: event end, thread %lu\n", log_prefix, index);
    __dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
    flexio_dev_cq_arm(_dev_ctx->rqcq_ctx.cq_idx, _dev_ctx->rqcq_ctx.cq_number);
    flexio_dev_thread_reschedule();
}

static void process_packet_reader(struct dev_ctx_worker* ctx, struct flexio_dev_thread_ctx *dtctx)
{
    uint32_t size;
    char *data = receive_packet(&ctx->rqcq_ctx, &ctx->rq_ctx, &size);
    __dpa_thread_memory_writeback();

    // Change destination MAC from throughput client MAC to DPA store MAC
    data[4] = 0x2c;
    data[5] = 0xa9;

    // Get pointer to DPAStore packet (after Ethernet + IP + UDP headers = 42 bytes)
    DPAStorePacket* pkt = (DPAStorePacket*)(data + 42);

    // Change operation type to FIND
    // pkt->client.control = PACKET_INIT_CONTROL(DPASTORE_OP_FIND, 0, 0);

    for (;;) {
        if (ctx->stop_sending) {
            return;
        }

        uint32_t bucket_id = pick_partition_bucket(ctx);
        const UniformPlan *plan = get_partition_plan(bucket_id);
        uint64_t key = 0ULL;
        uint8_t op_code = DPASTORE_OP_GET;
        pkt->value = 0ULL;

        uint16_t new_port;

        uint64_t mix;

        if (trace_mode_enabled()) {
            ctx->main_one_of_counter++;
            bool prefer_main = (g_runtime_state.main_trace_enabled &&
                                (ctx->main_one_of_counter % g_runtime_state.main_one_of == 0));
            bool use_main = prefer_main;
            if (!g_runtime_state.main_trace_enabled && g_runtime_state.loop_trace_enabled) {
                use_main = false;
            } else if (g_runtime_state.main_trace_enabled && !g_runtime_state.loop_trace_enabled) {
                use_main = true;
            } else if (!g_runtime_state.main_trace_enabled && !g_runtime_state.loop_trace_enabled) {
                use_main = false;
            }

            trace_cursor_state_t *cursor = use_main
                ? &g_trace_main_cursors[ctx->thread_index % DPASTORE_READER_THREAD_COUNT]
                : &g_trace_loop_cursors[ctx->thread_index % DPASTORE_READER_THREAD_COUNT];
            const uint64_t *trace_ptr = use_main ? ctx->main_trace_ptr : ctx->loop_trace_ptr;
            //ensure_trace_mapping(ctx);
            uint8_t stop_flag = 0;
            key = trace_next_key(ctx,
                                 use_main,
                                 cursor,
                                 trace_ptr,
                                 use_main ? g_runtime_state.main_trace_line_count : g_runtime_state.loop_trace_line_count,
                                 use_main ? g_runtime_state.main_trace_keys_per_line : g_runtime_state.loop_trace_keys_per_line,
                                 use_main ? (g_runtime_state.wrap_main != 0) : true,
                                 &stop_flag);
            if (stop_flag && use_main && !g_runtime_state.wrap_main) {
                ctx->stop_sending = 1;
                return;
            }
            if (trace_ptr == NULL) {
                /* Fallback if mapping failed: cycle partition keys */
                key = generate_uniform_key_for_plan(ctx, plan);
            }
            op_code = use_main ? g_runtime_state.main_trace_op : g_runtime_state.loop_trace_op;
            if (op_code == DPASTORE_OP_INSERT) {
                pkt->value = key + cursor->line * DPASTORE_READER_THREAD_COUNT + cursor->offset;
            }
            //uint32_t port_idx = (uint32_t)(((__uint128_t)mix * (uint64_t)DPASTORE_READER_THREAD_COUNT) >> 64);
        } else {
            uint64_t selector = prng_next(ctx);
            uint32_t sample = (uint32_t)((selector >> 48) & 0xFFFFU);
            bool do_insert = (sample < g_runtime_state.insert_threshold);
            if (do_insert) {
                key = generate_key_for_mode(ctx, plan, g_runtime_state.insert_distribution);
                pkt->value = key;
                op_code = DPASTORE_OP_INSERT;
            } else {
                key = generate_key_for_mode(ctx, plan, g_runtime_state.read_distribution);
                if (g_runtime_state.range_mode) {
                    op_code = DPASTORE_OP_RANGE;
                    pkt->value = 0ULL;
                }
            }
        }

        mix = key;
        mix ^= mix >> 33;
        mix *= 0xff51afd7ed558ccdULL;
        mix ^= mix >> 33;
        mix *= 0xc4ceb9fe1a85ec53ULL;
        mix ^= mix >> 33;
        new_port = (uint16_t)(DPASTORE_UDPPORT_READER + mix % DPASTORE_READER_THREAD_COUNT);

        ctx->current_key = key;
        pkt->key = key;
        pkt->control = PACKET_INIT_CONTROL(op_code, DPASTORE_STATUS_OK, 0);
        //pkt->reserved = 0;

        HotCachePlan cache_plan;
        hot_cache_plan_init_from_hash(&cache_plan, mix);
        pkt->hot_cache_plan = cache_plan;


        *PKT_UDP_DST_PORT(data) = cpu_to_be16(new_port);

#if DPASTORE_PERFORMANCE_COUNTING
        ctx->periodic_count++;
        ctx->periodic_total_count++;
#endif
        prepare_send_packet(&ctx->sq_ctx, data, size);
        finish_send_packet(&ctx->sq_ctx);
    }
}


void __dpa_global__ dpastore_device_event_handler_dpastore_reader(uint64_t index)
{
    dpastore_device_event_handler_worker(
        &dev_ctx_readers[index],
        process_packet_reader,
        "DPA reader",
        index
    );
}
