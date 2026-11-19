#ifndef PACKET_H
#define PACKET_H

#include "helpers.h"
#include "common/dpastore_common.h" // Include common packet definitions
#include <stdint.h>
#include "../shared/host_global_area.h"
#include "../shared/commands.h"
#include "../shared/server_config.h"
#include "../shared/hotcache.h"
#include "dpa_learned_tree_ops.h"
#include "../shared/tree_config.h"

// External declarations for host global area access
extern flexio_uintptr_t host_global_area_addr;
extern uint32_t host_global_window_id;
extern uint32_t host_global_mkey;

// Note: DPAStorePacket and related defines are now in dpastore_common.h


// --- UDP and MAC/IP helpers (unchanged) ---

static inline void set_udp_ports(char *data, uint16_t src_port, uint16_t dst_port)
{
    *(uint16_t *)(PKT_UDP_SRC(data)) = src_port;
    *(uint16_t *)(PKT_UDP_DST(data)) = dst_port;
}

static inline void rewrite_mac_ip(char *data, uint64_t key)
{
    size_t src_mac = *(size_t *)PKT_SRC_MAC(data);
    size_t dst_mac = *(size_t *)PKT_DST_MAC(data);
    *(size_t *)(PKT_SRC_MAC(data)) = dst_mac;
    *(size_t *)(PKT_DST_MAC(data)) = (src_mac & 0x0000FFFFFFFFFFFF) | (0x0008ll << 48);

    // *(int32_t *)(PKT_SRC_IP(data)) = cpu_to_be32(((uint32_t)key));
    // *(int32_t *)(PKT_SRC_IP(data)) = cpu_to_be32(SERVER_IP_HEX);
    // *(int32_t *)(PKT_DST_IP(data)) = cpu_to_be32(CLIENT_IP_HEX);
}

static inline void send_packet(struct dev_ctx_worker* ctx, char *data, uint32_t size, const char* msg)
{
    //LOG_I("%s\n", msg);
    //print_packet(data, size);
    prepare_send_packet(&ctx->sq_ctx, data, size);
    finish_send_packet(&ctx->sq_ctx);
}


static inline void send_packet_to_client(struct dev_ctx_worker* ctx, char *data, uint32_t size, uint64_t key) {
    // Reset MAC and IP addresses to send back to client
    rewrite_mac_ip(data, key);
    // Set UDP ports: server port as source, client source port as destination
    set_udp_ports(data, cpu_to_be16(DPASTORE_UDPPORT_READER), *(uint16_t *)PKT_UDP_SRC(data));
    
    // Skip UDP checksum for now
    *(uint16_t *)PKT_UDP_CHECK(data) = 0; // No checksum
    
    // Send packet
    send_packet(ctx, data, size, "DPA: Sending response packet back to client");
}

static inline void send_packet_to_updater(struct dev_ctx_worker* ctx, char *data, uint32_t size, uint32_t updater_id, bool is_initial) {
    // Keep MAC addresses for DPU internal routing
    // Set UDP ports: use updater thread ID + BASE_PORT as destination
    uint16_t dest_port = DPASTORE_UDPPORT_UPDATER + updater_id;
    set_udp_ports(data, cpu_to_be16(DPASTORE_UDPPORT_READER), cpu_to_be16(dest_port));
    
    // Skip UDP checksum for now
    *(uint16_t *)PKT_UDP_CHECK(data) = 0; // No checksum
    
    // Send packet
    char msg[128];
    if (is_initial) {
        snprintf(msg, sizeof(msg), "DPA: Forwarding packet to initial updater thread %u (port %u)", updater_id, dest_port);
    } else {
        snprintf(msg, sizeof(msg), "DPA: Forwarding packet to next updater thread %u (port %u)", updater_id, dest_port);
    }
    send_packet(ctx, data, size, msg);
}

static inline void resubmit_packet_to_reader(struct dev_ctx_worker* ctx, char *data, uint32_t size, DPAStorePacket* pkt) {
    uint8_t hop = PACKET_GET_HOP_COUNT(pkt->control);
    if (hop < 7) {
        hop++;
    }
    PACKET_SET_HOP_COUNT(pkt->control, hop);
    prepare_send_packet(&ctx->sq_ctx, data, size);
    finish_send_packet(&ctx->sq_ctx);
}

static inline bool dev_hot_cache_probe(dev_ctx_worker* ctx,
                                       const HotCachePlan* plan,
                                       uint64_t key,
                                       uint64_t* value_out)
{
    if (!hot_cache_bloom_has_plan(&ctx->hot_cache_bloom, plan)) {
        return false;
    }
    return hot_cache_table_lookup_plan(&ctx->hot_cache_table, key, plan, value_out);
}

static inline void dev_hot_cache_touch(dev_ctx_worker* ctx,
                                       const HotCachePlan* plan,
                                       uint64_t key,
                                       uint64_t value)
{
#if HOT_CACHE_USE_PROBABILITY_TRACKING
    if (ctx->hot_cache_control.hot_cache_admit_budget == 0U) {
        return;
    }

    if (!hot_cache_plan_should_admit(plan)) {
        return;
    }
#else
    (void)plan;
#endif
    hot_cache_table_insert_plan(&ctx->hot_cache_table, key, plan, value);
    hot_cache_bloom_add_plan(&ctx->hot_cache_bloom, plan);
#if HOT_CACHE_USE_PROBABILITY_TRACKING
    ctx->hot_cache_control.hot_cache_admit_budget--;
#else
    if (ctx->hot_cache_control.hot_cache_rebuild_countdown == 0U) {
        hot_cache_bloom_rebuild(&ctx->hot_cache_bloom, &ctx->hot_cache_table);
        ctx->hot_cache_control.hot_cache_rebuild_countdown = HOT_CACHE_BLOOM_REBUILD_INTERVAL;
    } else {
        ctx->hot_cache_control.hot_cache_rebuild_countdown--;
    }
#endif
}

static inline void dpa_process_get_with_hot_cache(dev_ctx_worker* ctx,
                                                  DPAStoreGlobalArea* global,
                                                  DPAStorePacket* pkt)
{

#if DPASTORE_ENABLE_HOT_CACHE
    // if (pkt->key % 100 >= 84){
    //     pkt->value = 42;
    //     PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_OK);
    //     return;
    // }

    // pkt->value = 42;
    // PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_OK);
    // return;

    uint64_t cached_value = 0;
    if (__builtin_expect(dev_hot_cache_probe(ctx, &pkt->hot_cache_plan, pkt->key, &cached_value), 1)) {
        pkt->value = cached_value;
        //printf("C");
        PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_OK);
#if !HOT_CACHE_USE_PROBABILITY_TRACKING
        dev_hot_cache_touch(ctx, &pkt->hot_cache_plan, pkt->key, cached_value);
#endif
        return;
    }

    uint64_t value = 0;
    bool found = dpa_learned_tree_find(&global->tree, pkt->key, &value, ctx->hga_dpa);
    if (found) {
        //printf("T");
        pkt->value = value;
        PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_OK);
        dev_hot_cache_touch(ctx, &pkt->hot_cache_plan, pkt->key, value);
    } else {
        //LOG_I("N pkt->key %lu\n", pkt->key);
        PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_NOTFOUND);
    }
#else
    uint64_t value = 0;
    bool found = dpa_learned_tree_find(&global->tree, pkt->key, &value, ctx->hga_dpa);
    if (found) {
        pkt->value = value;
        PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_OK);
    } else {
        PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_NOTFOUND);
    }
#endif
}

// Reader thread: builds path and forwards for INSERT, handles GET (non-batched version)
static void process_packet_reader(struct dev_ctx_worker* ctx, struct flexio_dev_thread_ctx *dtctx)
{
#if DPASTORE_TEST_MODE_DIRECT_RETURN == 1 && DPASTORE_PERFORMANCE_COUNTING == 1
    // Test mode: just count and return without any processing
    ctx->periodic_count++;
    if (ctx->periodic_count % 256*256 == 0)
        __dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
    return;
#endif


    uint32_t size;
    char *data = receive_packet(&ctx->rqcq_ctx, &ctx->rq_ctx, &size);
    __dpa_thread_fence(__DPA_MEMORY, __DPA_R, __DPA_R);

    DPAStorePacket* pkt = (DPAStorePacket*)PKT_CONTENT1(data);
    DPAStoreGlobalArea* global = (DPAStoreGlobalArea*)global_area_addr;
    size_t current_thread_id = ctx->thread_index;

    __builtin_prefetch(global->tree.root, 0, 0);
    
    // print_packet(data, size);

    // Store original client info if this is the first hop
    // Store client answer port from incoming packet
    // previous_updater_id doesn't exist in client packet - skip for now
    // PACKET_SET_HOP_COUNT(pkt->control, 0);
    
    // Extract op_type once to avoid multiple calculations
    uint8_t op_type = PACKET_GET_OP_TYPE(pkt->control);

    // Handle GET request
    if (op_type == DPASTORE_OP_GET) {
        //printf("G");

        // Access DPA global area and tree
        DPAStoreGlobalArea* global = (DPAStoreGlobalArea*)global_area_addr;
        // In production mode, assume tree is always initialized
        dpa_process_get_with_hot_cache(ctx, global, pkt);
        PACKET_SET_OP_TYPE(pkt->control, DPASTORE_OP_INTERNAL_OP_COMPLETE);
    } else if (op_type == DPASTORE_OP_RANGE) {
#if DPASTORE_RANGE_REQUEST
        uint16_t range_count = dpa_learned_tree_find_range(&global->tree,
                                                           pkt->key,
                                                           pkt->ranges,
                                                           DPASTORE_MAX_RANGE_RESULTS,
                                                           ctx->hga_dpa);
        pkt->value = range_count;
        if (range_count > 0) {
            PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_OK);
        } else {
            PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_NOTFOUND);
        }
        PACKET_SET_OP_TYPE(pkt->control, DPASTORE_OP_INTERNAL_OP_COMPLETE);
#else
        PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_ERROR);
        PACKET_SET_OP_TYPE(pkt->control, DPASTORE_OP_INTERNAL_OP_COMPLETE);
#endif
    } else if (op_type == DPASTORE_OP_INSERT) {
        //printf("I");
#if DEBUG_INSERTS_DPU_SIDE_FENCE_ATOMICS == 1
        //__dpa_thread_fence(__DPA_MMIO, __DPA_RW, __DPA_RW);
        //__dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW);
#endif
        //LOG_I("DPA: Received INSERT request for key=%lu, value=%lu\n", pkt->key, pkt->value);

        // Temp fix for single outliers
        if (pkt->key == 0 || pkt->key == 311718335922ULL) {
            LOG_I("DPA: Received INSERT request for key=%lu, value=%lu\n", pkt->key, pkt->value);
            return;
        }
        
        // Access DPA global area and tree
        DPAStoreGlobalArea* global = (DPAStoreGlobalArea*)global_area_addr;
        //if (global && global->tree.root) {
            DPALearnedLeafNode* leaf = NULL;
            dpa_insert_result_t result = dpa_learned_tree_insert(&global->tree, pkt->key, pkt->value, &leaf);

            if (result == DPA_INSERT_SUCCESS) {
                //printf("Is ");
                //LOG_I("DPA: Inserted key=%lu, value=%lu\n", pkt->key, pkt->value);
                PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_OK);
            } else if (result == DPA_INSERT_BUFFER_FULL && leaf != NULL) {
                //printf("If ");
                //LOG_I("DPA: Buffer full for key=%lu, sending patch to host\n", pkt->key);

                /* Send patch to host with full buffer contents */
                uint32_t patch_periodic_count = ctx->periodic_count;
                bool patch_sent = dpa_learned_send_buffer_full_patch(
                    leaf,
                    current_thread_id,
                    ctx->hga_dpa,
                    host_global_area_addr,
                    host_global_window_id,
                    host_global_mkey,
                    patch_periodic_count);

                if (patch_sent) {
                    ctx->insert_count += LEARNED_INSERT_BUFFER_SIZE;
                    PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_RETRY_FROM_ROOT);
                } else {
                    //LOG_I("DPA: Failed to send buffer full patch\n");
                    PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_ERROR);
                }
            } else if (result == DPA_INSERT_UPDATE_IN_PROGRESS) {
                //resubmit_packet_to_reader(ctx, data, size, pkt);
                //printf("Ip ");
                return;
            } else {
                LOG_I("DPA: Failed to insert key=%lu (error)\n", pkt->key);
                PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_ERROR);
            }
        // } else {
        //     LOG_I("DPA: Tree not initialized\n");
        //     PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_ERROR);
        // }
        
        PACKET_SET_OP_TYPE(pkt->control, DPASTORE_OP_INTERNAL_OP_COMPLETE);
    } else if (op_type == DPASTORE_OP_PING) {

        //LOG_I("PING\n");

        PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_OK); // Indicate success
        PACKET_SET_OP_TYPE(pkt->control, DPASTORE_OP_INTERNAL_OP_COMPLETE);
        
    }else {

        LOG_I("NOP\n");

        // Other operations not yet implemented
        PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_ERROR);
        PACKET_SET_OP_TYPE(pkt->control, DPASTORE_OP_INTERNAL_OP_COMPLETE);
    }

#if DPASTORE_PERFORMANCE_COUNTING == 1
        // Count just before sending response (or where response would be sent)
        ctx->periodic_count++;
#if DPASTORE_RESPONSE_MODE == RESPONSE_NONE
        // Add fence only if we're not sending (sending includes its own fence)
            __dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
#endif

#if DPASTORE_RESPONSE_MODE != RESPONSE_NONE
        send_packet_to_client(ctx, data, size, pkt->key);
#endif
        

#endif
}

// Updater thread: Simplified for packet interception only
static void process_packet_updater(struct dev_ctx_worker* ctx, struct flexio_dev_thread_ctx *dtctx)
{
    LOG_I("DPA: process_packet_updater\n");

    uint32_t size;
    char *data = receive_packet(&ctx->rqcq_ctx, &ctx->rq_ctx, &size);
    __dpa_thread_fence(__DPA_MEMORY, __DPA_R, __DPA_R);
    if (!data) return;

    DPAStorePacket* pkt = (DPAStorePacket*)PKT_CONTENT1(data);
    DPAStoreGlobalArea* global = (DPAStoreGlobalArea*)global_area_addr;
    size_t current_thread_id = ctx->thread_index;

    LOG_I("DPA: Updater thread %u received packet\n", current_thread_id);
    print_packet(data, size);
    
#if DPASTORE_PERFORMANCE_COUNTING == 1
    // Count packet
    ctx->periodic_count++;
#endif

    // For now, just respond with OK
    PACKET_SET_STATUS(pkt->control, DPASTORE_STATUS_OK);
    PACKET_SET_OP_TYPE(pkt->control, DPASTORE_OP_INTERNAL_OP_COMPLETE);
    
#if DPASTORE_RESPONSE_MODE != RESPONSE_NONE
    send_packet_to_client(ctx, data, size, pkt->key);
#endif
}

#endif // PACKET_H
