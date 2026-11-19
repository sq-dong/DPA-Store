#include <stddef.h>
#include "wrapper_flexio_device.h"
#include "common/dpa_common.h"
#include "common/dpastore_common_config.h"
#include "../shared/commands.h"
#include "../shared/ringbuffer.h"
#include "../shared/host_global_area.h"
#include "../shared/hotcache.h"
#include "stitcher.h"

// --- Type definitions ---
// IMPORTANT: PACKED AND ALIGNED 64 attributed are HARMFULL for performance!
typedef struct dev_ctx_worker
{
    uint32_t lkey;
    uint32_t is_initalized;
    struct cq_ctx_t rqcq_ctx;
    struct cq_ctx_t sqcq_ctx;
    struct rq_ctx_t rq_ctx;
    struct sq_ctx_t sq_ctx;
    //struct dt_ctx_t dt_ctx;
    struct host_rq_ctx_t host_rq_ctx;
    uint32_t periodic_count;         // Packet count for current window
    uint32_t insert_count;           // Accumulated inserted keys in current window
    //uint8_t sq_on_host;
    uint8_t thread_index;
    //uint8_t reserved[7];
    host_global_area_t* hga_dpa;
    HotCacheBloom hot_cache_bloom;   // Bloom lives alongside frequently accessed ctx metadata
    union {
        uint32_t hot_cache_admit_budget;
    } hot_cache_control;

    HotCacheTable hot_cache_table;
} dev_ctx_worker __attribute__((aligned(64)));


// --- Global variables ---

struct dev_ctx_worker dev_ctx_readers[DPASTORE_READER_THREAD_COUNT] = {0};
struct dev_ctx_worker dev_ctx_updaters[DPASTORE_UPDATER_THREAD_COUNT] = {0};

// Global area pointer and memory key (set by host via RPC)
flexio_uintptr_t global_area_addr = 0;
uint32_t global_area_mkey = 0;

// Host global area for ring buffer communication
flexio_uintptr_t host_global_area_addr = 0;
uint32_t host_global_window_id = 0;
uint32_t host_global_mkey = 0;

// Stitcher thread event handler ID
uint32_t stitcher_activation_ids[NUM_STITCH_QUEUES] = {0};

#if DPASTORE_PERFORMANCE_COUNTING
// Accumulators for throughput statistics across all reporting windows
uint64_t global_packet_count_total = 0;
uint64_t global_insert_count_total = 0;

// Function to print and reset periodic throughput stats
// Returns total packets processed in the window
__dpa_rpc__ uint64_t print_and_reset_periodic_throughput(uint64_t elapsed_ms) {
    uint32_t elapsed_seconds_int = elapsed_ms / 1000;
    uint32_t elapsed_ms_frac = elapsed_ms % 1000;


    __dpa_thread_fence(__DPA_MEMORY, __DPA_R, __DPA_R);
    __dpa_thread_fence(__DPA_HEAP, __DPA_R, __DPA_R);
    
    flexio_dev_print("DPA: === Throughput Stats (%u.%03us window) ===\n", 
                     elapsed_seconds_int, elapsed_ms_frac);
    
    uint64_t total_window_packets = 0;
    uint64_t total_window_inserts = 0;

    for (int i = 0; i < DPASTORE_READER_THREAD_COUNT; i++) {
        struct dev_ctx_worker* ctx = &dev_ctx_readers[i];
        // Print per-thread statistics (optional)
        // uint64_t thread_pps = (ctx->periodic_count * 1000) / elapsed_ms;
        // uint64_t thread_mops = thread_pps / 1000000;
        // uint64_t thread_mops_frac = (thread_pps % 1000000) / 1000;
        // flexio_dev_print("DPA: Thread %d: %lu pps (%lu.%03lu Mops) (window: %lu packets)\n",
        //     i, thread_pps, thread_mops, thread_mops_frac, ctx->periodic_count);

        total_window_packets += ctx->periodic_count;
        total_window_inserts += ctx->insert_count;

        // Reset window counter
        ctx->periodic_count = 0;
        ctx->insert_count = 0;
    }

    global_packet_count_total += total_window_packets;
    global_insert_count_total += total_window_inserts;
    
    if (total_window_packets > 0 && elapsed_ms > 0) {
        // Calculate packets per second: (packets * 1000ms) / elapsed_ms
        uint64_t pps = (total_window_packets * 1000) / elapsed_ms;
        uint64_t mops = pps / 1000000;  // Mega operations per second
        uint64_t mops_frac = (pps % 1000000) / 1000;  // Fractional part in thousands
        flexio_dev_print("DPA: Total: %lu pps (%lu.%03lu Mops) (%u.%03us window: %lu packets)\n",
            pps, mops, mops_frac, elapsed_seconds_int, elapsed_ms_frac,
            total_window_packets);
    } else if (total_window_packets == 0) {
        flexio_dev_print("DPA: No packets processed in this window\n");
    }

    if (total_window_inserts > 0 && elapsed_ms > 0) {
        uint64_t ips = (total_window_inserts * 1000) / elapsed_ms;
        uint64_t mkeys = ips / 1000000;
        uint64_t mkeys_frac = (ips % 1000000) / 1000;
        flexio_dev_print("DPA: Inserted keys: %lu keys/s (%lu.%03lu Mkeys) (%u.%03us window: %lu keys)\n",
                         ips, mkeys, mkeys_frac, elapsed_seconds_int, elapsed_ms_frac,
                         total_window_inserts);
    } else if (total_window_inserts == 0) {
        flexio_dev_print("DPA: No insert-buffer patches in this window\n");
    }

    flexio_dev_print("DPA: Cumulative packets processed: %lu\n", global_packet_count_total);
    flexio_dev_print("DPA: Cumulative inserts applied: %lu\n", global_insert_count_total);

    __dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
    __dpa_thread_fence(__DPA_HEAP, __DPA_W, __DPA_W);

    flexio_dev_print("DPA: === End Stats ===\n");

    return total_window_packets;
}
#endif // DPASTORE_PERFORMANCE_COUNTING


#include "dpastore_global_area.h"
// Note: packet_batched.h includes packet.h internally
// #include "packet_batched.h"  // Include batched packet processing
#include "packet.h"

static inline void dev_hot_cache_init(dev_ctx_worker* ctx)
{
    hot_cache_bloom_clear(&ctx->hot_cache_bloom);
    hot_cache_table_init(&ctx->hot_cache_table);
#if HOT_CACHE_USE_PROBABILITY_TRACKING
    ctx->hot_cache_control.hot_cache_admit_budget = HOT_CACHE_PROB_MAX_ADMITS;
#else
    ctx->hot_cache_control.hot_cache_rebuild_countdown = HOT_CACHE_BLOOM_REBUILD_INTERVAL;
#endif
}

// --- RPC to set global area pointer ---

__dpa_rpc__ uint64_t dpastore_device_set_global_area(uint64_t data)
{
    LOG_I("DPA: Entering dpastore_device_set_global_area\n");
    global_area_addr = (flexio_uintptr_t)data;
    global_area_mkey = 0; // Assuming device memory, mkey is 0
    LOG_I("DPA: Global area pointer set to %p\n", (void*)global_area_addr);

    global_area_init();

    LOG_I("DPA: Exiting dpastore_device_set_global_area\n");

    size_t thread_index_offset = offsetof(dev_ctx_worker, thread_index);
    LOG_I("DPA: dev_ctx_worker.thread_index offset = %zu bytes\n", thread_index_offset);

    size_t hot_cache_bloom_offset = offsetof(dev_ctx_worker, hot_cache_bloom);
    LOG_I("DPA: dev_ctx_worker.hot_cache_bloom offset = %zu bytes\n", hot_cache_bloom_offset);

    
    LOG_I("DPA: sizeof(HotCacheBloom) = %zu bytes\n", sizeof(HotCacheBloom));
    LOG_I("DPA: sizeof(HotCacheTable) = %zu bytes\n", sizeof(HotCacheTable));

    return 0; // Success
}

// --- Stitcher thread event handler ---

flexio_dev_event_handler_t dpastore_device_stitcher_thread;
__dpa_global__ void dpastore_device_stitcher_thread(uint64_t thread_arg)
{
    uint32_t queue_id = (uint32_t)(thread_arg & 0xFFFFFFFFu);

    uint32_t activation_id = (queue_id < NUM_STITCH_QUEUES) ? stitcher_activation_ids[queue_id] : 0;

    LOG_I("DPA: Stitcher thread started for queue %u (activation_id=%u, arg=%lu)\n",
          queue_id, activation_id, thread_arg);

    if (queue_id >= NUM_STITCH_QUEUES) {
        LOG_I("DPA: Stitcher thread received invalid queue id %u\n", queue_id);
        flexio_dev_thread_finish();
        return;
    }
    
    // Check if we have valid host global area
    if (host_global_area_addr == 0 || host_global_window_id == 0) {
        LOG_I("DPA: Stitcher thread - no host global area configured, exiting\n");
        flexio_dev_thread_finish();
        return;
    }
    
    // Configure window to access host memory
    flexio_dev_window_config(FLEXIO_DEV_WINDOW_ENTITY_0, host_global_window_id, host_global_mkey);
    
    // Get pointer to host global area through window
    host_global_area_t *hga;
    flexio_uintptr_t dpa_addr;
    flexio_dev_status_t status = flexio_dev_window_ptr_acquire(FLEXIO_DEV_WINDOW_ENTITY_0, 
                                                               host_global_area_addr, &dpa_addr);
    
    if (status != FLEXIO_DEV_STATUS_SUCCESS) {
        LOG_I("DPA: Stitcher thread - failed to acquire window pointer, status=%d\n", status);
        flexio_dev_thread_finish();
        return;
    }
    
    hga = (host_global_area_t *)dpa_addr;
    LOG_I("DPA: Stitcher thread - window acquired, hga=%p\n", (void*)hga);
    
    // Main receive loop for stitch commands - using in-place consumption
    void *cmd_ptr;
    uint32_t cmd_size;
    uint32_t loop_count = 0;
    uint32_t check_count = 0;

    LOG_I("DPA: Stitcher entering main loop (in-place mode)\n");

    while (1) {

            __dpa_thread_fence(__DPA_MMIO, __DPA_RW, __DPA_RW); // TODO
            __dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW); // TODO

        // Try to peek at next stitch command from host (no copy)
        if (ringbuffer_peek(&hga->stitch_queue_metadata[queue_id], hga->stitch_queue_buffers[queue_id],
                           STITCH_QUEUE_SIZE, &cmd_ptr, &cmd_size)) {

            //__dpa_thread_fence(__DPA_MMIO, __DPA_RW, __DPA_RW); // TODO
            //__dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW); // TODO

            // Process the stitch command directly in the ringbuffer

            process_stitch_command((command_generic_t *)cmd_ptr, hga, thread_arg);

            // __dpa_thread_fence(__DPA_MMIO, __DPA_RW, __DPA_RW); // TODO
            // __dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW); // TODO

            // Commit the consumption to advance the ringbuffer tail
            ringbuffer_consume_commit(&hga->stitch_queue_metadata[queue_id], STITCH_QUEUE_SIZE, cmd_size);

            // __dpa_thread_fence(__DPA_MMIO, __DPA_RW, __DPA_RW);
            // __dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW);

            // Check if we should exit after root update
            // if (root_update_received) {
            //     LOG_I("DPA: Stitcher thread exiting - tree root has been set\n");
            //     flexio_dev_thread_finish();
            //     return;
            // }

        }
        
        // // Periodic status log
        // if (++check_count % 1000000 == 0) {
        //     __dpa_thread_window_read_inv();
        // }
        
        // // Periodically reschedule to avoid blocking other threads
        // if (++loop_count % 1000 == 0) {
        //     flexio_dev_thread_reschedule();
        // }
    }
}

// --- RPC to initialize and start stitcher thread ---

__dpa_rpc__ uint64_t dpastore_device_init_stitcher(uint64_t data)
{
    uint32_t activation_id = (uint32_t)(data & 0xFFFFFFFFu);
    uint32_t queue_id = (uint32_t)((data >> 32) & 0xFFFFFFFFu);

    if (queue_id >= NUM_STITCH_QUEUES) {
        LOG_I("DPA: Invalid stitcher queue id %u (activation_id=%u)\n", queue_id, activation_id);
        return 1;
    }

    LOG_I("DPA: Initializing stitcher thread for queue %u (activation id %u)\n",
          queue_id, activation_id);

    stitcher_activation_ids[queue_id] = activation_id;
    flexio_dev_event_handler_activate(activation_id);

    LOG_I("DPA: Stitcher thread %u activated\n", queue_id);

    return 0; // Success
}

// --- RPC to set host global area window info ---

__dpa_rpc__ uint64_t dpastore_device_set_host_global_area(uint64_t data)
{
    struct {
        flexio_uintptr_t host_global_area_addr;
        uint32_t window_id;
        uint32_t mkey;
    } *window_info = (void*)data;
    
    LOG_I("DPA: Setting host global area window info\n");
    host_global_area_addr = window_info->host_global_area_addr;
    host_global_window_id = window_info->window_id;
    host_global_mkey = window_info->mkey;
    
    LOG_I("DPA: Host global area: addr=%p, window_id=%u, mkey=0x%x\n", 
          (void*)host_global_area_addr, host_global_window_id, host_global_mkey);

    LOG_I("DPA: dev_ctx_worker=%zu bytes\n", sizeof(dev_ctx_worker));
    
    return 0; // Success
}

// --- CQ, RQ, SQ initialization ---

static void init_cq(const struct app_transfer_cq app_cq, struct cq_ctx_t *ctx)
{
    ctx->cq_number = app_cq.cq_num;
    ctx->cq_ring = (struct flexio_dev_cqe64 *)app_cq.cq_ring_daddr;
    ctx->cq_dbr = (uint32_t *)app_cq.cq_dbr_daddr;
    ctx->cqe = ctx->cq_ring;
    ctx->cq_idx = 0;
    ctx->cq_hw_owner_bit = 0x1;
    ctx->cq_idx_mask = ((1 << LOG_CQ_RING_DEPTH) - 1);
}

static void init_rq(const struct app_transfer_wq app_rq, struct rq_ctx_t *ctx)
{
    ctx->rq_number = app_rq.wq_num;
    ctx->rq_ring = (struct flexio_dev_wqe_rcv_data_seg *)app_rq.wq_ring_daddr;
    ctx->rq_dbr = (uint32_t *)app_rq.wq_dbr_daddr;
    ctx->rq_idx_mask = ((1 << LOG_RQ_RING_DEPTH) - 1);
}

static void init_sq(const struct app_transfer_wq app_sq, struct sq_ctx_t *ctx)
{
    ctx->sq_number = app_sq.wq_num;
    ctx->sq_ring = (union flexio_dev_sqe_seg *)app_sq.wq_ring_daddr;
    ctx->sq_dbr = (uint32_t *)app_sq.wq_dbr_daddr;
    ctx->sq_wqe_seg_idx = 0;
    ctx->sq_dbr++;
    ctx->sq_idx_mask = ((1 << (LOG_SQ_RING_DEPTH + LOG_SQE_NUM_SEGS)) - 1);
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

    init_send_sq(&_dev_ctx->sq_ctx, LOG2VALUE(LOG_SQ_RING_DEPTH), _dev_ctx->lkey);
    // _dev_ctx->dt_ctx.sq_tx_buff = (void *)shared_data->sq_data.wqd_daddr;
    // _dev_ctx->dt_ctx.tx_buff_idx = 0;
    // _dev_ctx->dt_ctx.data_idx_mask = ((1 << (LOG_SQ_RING_DEPTH)) - 1);

    //_dev_ctx->sq_on_host = shared_data->sq_data.daddr_on_host;

    dev_hot_cache_init(_dev_ctx);

    _dev_ctx->is_initalized = 1;
    return 0;
}

__dpa_rpc__ uint64_t dpastore_device_init_dpastore_reader(uint64_t data)
{
    struct queue_config_data *shared_data = (struct queue_config_data *)data;
    uint32_t thread_index = shared_data->thread_index;
    
    if (thread_index >= DPASTORE_READER_THREAD_COUNT) {
        flexio_dev_print("ERROR: thread_index %u >= DPASTORE_READER_THREAD_COUNT %u\n", 
                         thread_index, DPASTORE_READER_THREAD_COUNT);
        return 1;
    }
    
    struct dev_ctx_worker* _dev_ctx = &dev_ctx_readers[thread_index];
    _dev_ctx->thread_index = (uint8_t)thread_index;  // Save thread index in context

    uint64_t ret = dpastore_device_init_worker(_dev_ctx, shared_data);
    flexio_dev_print("DPA reader: device init complete, thread %u listening on port %u\n", 
                     thread_index, DPASTORE_UDPPORT_READER + thread_index);
    return ret;
}

__dpa_rpc__ uint64_t dpastore_device_init_dpastore_updater(uint64_t data)
{
    struct queue_config_data *shared_data = (struct queue_config_data *)data;
    uint32_t thread_index = shared_data->thread_index;
    struct dev_ctx_worker* _dev_ctx = &dev_ctx_updaters[thread_index];
    
    _dev_ctx->thread_index = (uint8_t)thread_index;  // Save thread index in context

    uint64_t ret = dpastore_device_init_worker(_dev_ctx, shared_data);
    flexio_dev_print("DPA updater: device init, thread %u listening on port %u\n", 
                     thread_index, DPASTORE_UDPPORT_UPDATER + thread_index);
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

    // Initialize hga_dpa if not already done
    if (_dev_ctx->hga_dpa == NULL && host_global_area_addr != 0 && host_global_window_id != 0) {
        flexio_dev_window_config(FLEXIO_DEV_WINDOW_ENTITY_0, host_global_window_id, host_global_mkey);
        flexio_uintptr_t dpa_addr;
        flexio_dev_status_t status = flexio_dev_window_ptr_acquire(FLEXIO_DEV_WINDOW_ENTITY_0,
                                                                   host_global_area_addr, &dpa_addr);
        if (status == FLEXIO_DEV_STATUS_SUCCESS) {
            _dev_ctx->hga_dpa = (host_global_area_t *)dpa_addr;
        }
    }

    LOG_I("%s: event begin, thread %lu\n", log_prefix, index);
    __dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW);

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

void __dpa_global__ dpastore_device_event_handler_dpastore_reader(uint64_t index)
{
    // Use batched packet processing
    dpastore_device_event_handler_worker(
        &dev_ctx_readers[index],
        process_packet_reader,  // Use batched version
        "DPA reader",
        index
    );
}

void __dpa_global__ dpastore_device_event_handler_dpastore_updater(uint64_t index)
{
    dpastore_device_event_handler_worker(
        &dev_ctx_updaters[index],
        process_packet_updater,
        "DPA updater",
        index
    );
}
