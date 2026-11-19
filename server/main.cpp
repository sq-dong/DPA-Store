#include "dpastore.hpp"
#include "common/dpastore_common.h" // Include for test_data_buffer_info

DOCA_LOG_REGISTER(DPA_STORE_X86);

#include "shared/server_config.h"
#include "host_global_area_mgmt.h"
#include "host_tree/patcher.hpp"
#include "host_tree/host_learned_tree.hpp"
#include "../common/workload_config.hpp"
#include "../common/partition_layout.hpp"
#include "../common/uniform_catalog.h"
#include "test_data_generator.hpp"

#include <stdio.h>
#include <chrono>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <random>
#include <vector>

#include "shared/commands.h"
#include "shared/ringbuffer.h"
#include "shared/host_global_area.h"
#include "device/dpa_learned_tree_types.h"
#include "trace_load.hpp"



// Helper function to allocate multiple DPA segments
std::vector<void*> allocate_dpa_segments(FLEX::Context* ctx, size_t segment_size, size_t target_segments) {
    std::vector<void*> dpa_segments;
    dpa_segments.reserve(target_segments);

    DOCA_LOG_INFO("Allocating %zu DPA segments of %zu MB each",
                  target_segments, segment_size / (1024 * 1024));

    for (size_t i = 0; i < target_segments; i++) {
        flexio_uintptr_t segment_addr;
        if (flexio_buf_dev_alloc(ctx->get_process(), segment_size, &segment_addr) != FLEXIO_STATUS_SUCCESS) {
            DOCA_LOG_WARN("Failed to allocate DPA segment %zu, continuing with %zu segments", i, i);
            break;
        }
        dpa_segments.push_back(reinterpret_cast<void*>(segment_addr));
        DOCA_LOG_INFO("DPA segment %zu allocated at device address: %p", i, (void*)segment_addr);
    }

    if (dpa_segments.empty()) {
        DOCA_LOG_ERR("Failed to allocate any DPA segments");
        return {};
    }

    DOCA_LOG_INFO("Successfully allocated %zu DPA segments (%.2f GB total)",
                  dpa_segments.size(),
                  (dpa_segments.size() * segment_size) / (1024.0 * 1024.0 * 1024.0));

    return dpa_segments;
}

static bool force_quit;

static void
signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        /* Add additional new lines for output readability */
        DOCA_LOG_INFO(" ");
        DOCA_LOG_INFO("Signal %d received, preparing to exit", signum);
        DOCA_LOG_INFO(" ");
        force_quit = true;
    }
}

static void
print_tree_info(HostLearnedTree* host_tree) {
    DOCA_LOG_INFO("=== Tree Information ===");
    DOCA_LOG_INFO("  - Total entries: %lu", host_tree->get_num_entries());
    DOCA_LOG_INFO("  - Leaf nodes: %lu", host_tree->get_num_leafs());
    DOCA_LOG_INFO("  - Inner nodes: %lu", host_tree->get_num_inners());
    DOCA_LOG_INFO("  - Tree height: %zu", host_tree->get_height());
    size_t counted_leaves = 0;
    size_t counted_inners = 0;
    size_t total_leaf_entries = 0;
    size_t total_inner_children = 0;
    size_t total_inner_segments = 0;

    HostLearnedTreeNode* root = host_tree->get_root();
    if (root) {
        std::vector<HostLearnedTreeNode*> stack;
        stack.push_back(root);
        while (!stack.empty()) {
            HostLearnedTreeNode* node = stack.back();
            stack.pop_back();
            if (!node) {
                continue;
            }

            if (node->type == HostLearnedTreeNode::LEARNED_LEAF) {
                auto* leaf = static_cast<HostLearnedLeafNode*>(node);
                counted_leaves++;
                total_leaf_entries += static_cast<size_t>(leaf->sorted_count) +
                                      static_cast<size_t>(leaf->buffer_count);
            } else {
                auto* inner = static_cast<HostLearnedInnerNode*>(node);
                counted_inners++;
                total_inner_children += inner->num_children;
                total_inner_segments += inner->num_segments;
                for (uint16_t i = 0; i < inner->num_children; i++) {
                    stack.push_back(inner->children[i]);
                }
            }
        }
    }

    double avg_entries_per_leaf =
        counted_leaves ? static_cast<double>(total_leaf_entries) / counted_leaves : 0.0;
    double avg_children_per_inner =
        counted_inners ? static_cast<double>(total_inner_children) / counted_inners : 0.0;
    double avg_segments_per_inner =
        counted_inners ? static_cast<double>(total_inner_segments) / counted_inners : 0.0;

    DOCA_LOG_INFO("  - Avg entries per leaf: %.2f", avg_entries_per_leaf);
    DOCA_LOG_INFO("  - Avg children per inner: %.2f", avg_children_per_inner);
    DOCA_LOG_INFO("  - Avg segments per inner: %.2f", avg_segments_per_inner);
    DOCA_LOG_INFO("  Host pools:");
    DOCA_LOG_INFO("    - Leaf pool usage: %zu MB", host_tree->get_leaf_pool_usage() / 1024 / 1024);
    DOCA_LOG_INFO("    - Inner pool usage: %zu MB", host_tree->get_inner_pool_usage() / 1024 / 1024);
    DOCA_LOG_INFO("  DPA pools:");
    DOCA_LOG_INFO("    - Leaf pool usage: %zu MB", host_tree->get_dpa_leaf_pool_usage() / 1024 / 1024);
    DOCA_LOG_INFO("    - Inner pool usage: %zu MB", host_tree->get_dpa_inner_pool_usage() / 1024 / 1024);
}

static void
print_dpa_stats(dpastore_config& config, flexio_func_t& print_and_reset_periodic_throughput,
                std::chrono::steady_clock::time_point& last_stats_time) {
    uint64_t rpc_ret_val = 0;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_stats_time);
    double elapsed_seconds = elapsed.count() / 1000.0;
    uint64_t elapsed_ms = elapsed.count();

    DOCA_LOG_INFO("=== DPA Statistics (%.2f seconds since last check) ===", elapsed_seconds);
    if (flexio_process_call(config.ctx->get_process(), &print_and_reset_periodic_throughput, &rpc_ret_val, elapsed_ms) == FLEXIO_STATUS_SUCCESS) {
        // Stats printed by device (DPA uses elapsed_ms for throughput calculation)
    } else {
        DOCA_LOG_WARN("Failed to call device stats printing function");
    }

    last_stats_time = now;
}

// Helper function to initialize a worker thread (reader or updater)
void init_worker_thread(
    dpastore_config_worker_thread* wt,
    dpastore_config* config,
    FLEX::flow_matcher& matcher,
    uint32_t thread_idx,
    uint32_t current_thread,
    flexio_func_t* event_handler_func,
    flexio_func_t* device_init_func,
    uint16_t udp_port_base
) {
    config->ctx->create_event_handler(*event_handler_func, 0, FLEX::Context::EventHandlerAffinity::Strict);

    wt->rq_cq = new FLEX::CQ(true, LOG_CQ_RING_DEPTH, config->ctx, current_thread);
    wt->sq_cq = new FLEX::CQ(false, LOG_CQ_RING_DEPTH, config->ctx, current_thread);
    wt->rq = new FLEX::RQ(LOG_RQ_RING_DEPTH, LOG_RQ_DATA_ENTRY_BSIZE, wt->rq_cq->get_cq_num(), config->ctx, config->rq_buffer_on_host);
    wt->sq = new FLEX::SQ(LOG_SQ_RING_DEPTH, LOG_SQ_DATA_ENTRY_BSIZE, wt->sq_cq->get_cq_num(), config->ctx, wt->rq);

    queue_config_data dev_data {
        wt->rq_cq->get_cq_transf(),
        wt->rq->get_rq_transf(),
        wt->sq_cq->get_cq_transf(),
        wt->sq->get_sq_transf(),
        thread_idx, 0, config->ctx->get_window_id()
    };
    flexio_uintptr_t dev_config_data = config->ctx->move_to_dev(dev_data);
    uint64_t rpc_ret_val;
    Assert(flexio_process_call(config->ctx->get_process(), device_init_func, &rpc_ret_val, dev_config_data) == FLEXIO_STATUS_SUCCESS);

    matcher.clear();
    wt->rx_flow_rule = new FLEX::dr_flow_rule();
    wt->rx_flow_rule->add_dest_devx_tir(wt->rq->get_inner_ptr());
    matcher.set_dst_mac(SERVER_MAC_HEX);
    matcher.set_dst_udp_port((uint16_t)(udp_port_base + thread_idx));
    wt->rx_flow_rule->create_dr_rule(config->global_rx_flow_table, &matcher);

    matcher.clear();
    wt->tx_flow_rule = new FLEX::dr_flow_rule();
    wt->tx_flow_root_rule = new FLEX::dr_flow_rule();
    wt->tx_flow_root_rule->add_dest_table(config->global_tx_flow_table->dr_table);
    wt->tx_flow_rule->add_dest_vport(config->global_tx_dr->get_inner_ptr(), 0xFFFF);

    matcher.set_src_mac(SERVER_MAC_HEX);

    #define NIC_MODE true

    if (!NIC_MODE) {
        wt->tx_flow_root_rule->create_dr_rule(config->global_tx_flow_root_table, &matcher);
    } else {
        (void)config->global_tx_flow_root_table;
    }

    wt->tx_flow_rule->create_dr_rule(config->global_tx_flow_table, &matcher);

    config->ctx->event_handler_run(thread_idx, current_thread);
}

int main(int argc, char **argv) {
    SET_LOG_LEVEL(DOCA_LOG_LEVEL_INFO);

    DOCA_LOG_INFO("=== DPA-Store X86 Server Starting ===");

    // Print node sizes
    DOCA_LOG_INFO("=== Host Node Structure Sizes ===");
    DOCA_LOG_INFO("  HostLearnedTreeNode: %zu bytes", sizeof(HostLearnedTreeNode));
    DOCA_LOG_INFO("  HostLearnedLeafNode: %zu bytes", sizeof(HostLearnedLeafNode));
    DOCA_LOG_INFO("  HostLearnedInnerNode: %zu bytes", sizeof(HostLearnedInnerNode));
    DOCA_LOG_INFO("  HostLearnedLeafNode -> sorted_keys: %zu bytes", offsetof(HostLearnedLeafNode, sorted_keys));
    DOCA_LOG_INFO("  LearnedSegment: %zu bytes", sizeof(LearnedSegment));
     DOCA_LOG_INFO("  command_patch_buffer_full_t: %zu bytes", sizeof(command_patch_buffer_full_t));
    DOCA_LOG_INFO("  KVPair: %zu bytes", sizeof(KVPair));
    DOCA_LOG_INFO("  DPAStoreClientPacket: %zu bytes", sizeof(DPAStoreClientPacket));

    DOCA_LOG_INFO("=== DPA Node Structure Sizes ===");
    DOCA_LOG_INFO("  DPALearnedLeafNode: %zu bytes", sizeof(DPALearnedLeafNode));
    DOCA_LOG_INFO("  DPALearnedInnerNode: %zu bytes", sizeof(DPALearnedInnerNode));
    DOCA_LOG_INFO("  DPALearnedSegment: %zu bytes", sizeof(DPALearnedSegment));
    DOCA_LOG_INFO("  DPAKVPair: %zu bytes", sizeof(DPAKVPair));

    DOCA_LOG_INFO("=== Shared Configuration ===");
    DOCA_LOG_INFO("  Max segments per inner: %d", LEARNED_SEGMENTS_PER_INNER);
    DOCA_LOG_INFO("  Max keys per segment: %d", LEARNED_KEYS_PER_INNER_SEGMENT);
    DOCA_LOG_INFO("  Max children per inner: %d", LEARNED_SEGMENTS_PER_INNER * LEARNED_KEYS_PER_INNER_SEGMENT);
    DOCA_LOG_INFO("  Max keys per leaf: %d", LEARNED_KEYS_PER_LEAF);

    dpastore_config config;
    config.device_name = SERVER_DEVICE_NAME;

    DOCA_LOG_INFO("Initializing FlexIO device context for: %s", config.device_name.c_str());

    config.ctx = new FLEX::Context(config.device_name);
    Assert(config.ctx);
    DOCA_LOG_INFO("FlexIO context created");

    flexio_version_set(FLEXIO_CURRENT_VERSION);

    // // useless at DOCA2.2.1
    // // config.ctx->alloc_devx_uar(0x1);
    DOCA_LOG_INFO("Allocating protection domain");
    config.ctx->alloc_pd();

    DOCA_LOG_INFO("Creating DPA process with device code");
    config.ctx->create_process(dpastore_dev);

    DOCA_LOG_INFO("Creating FlexIO window");
    config.ctx->create_window();

    DOCA_LOG_INFO("Generating FlexIO UAR");
    config.ctx->generate_flexio_uar();
    config.ctx->print_init("dpastore", 0);

    /* Initialize Host Global Area for host-DPA communication */
    DOCA_LOG_INFO("=== Initializing Host Global Area ===");
    host_global_area_t *host_global_area = nullptr;
    struct ibv_mr *host_global_mr = nullptr;

    if (!host_global_area_create(config.ctx->get_pd(),
                                  sizeof(host_global_area_t),
                                  &host_global_area,
                                  &host_global_mr)) {
        DOCA_LOG_ERR("Failed to create host global area");
        return EXIT_FAILURE;
    }

    DOCA_LOG_INFO("Host Global Area initialized successfully");
    DOCA_LOG_INFO("  Memory key: 0x%x", host_global_mr->lkey);
    DOCA_LOG_INFO("  Window ID: %u", config.ctx->get_window_id());

    // Note: Window is already created by config.ctx->create_window()
    // DPA will use the window_id and mkey to access host memory

    // Allocate multiple DPA segments for full memory access
    DOCA_LOG_INFO("=== Allocating Multiple DPA Segments ===");

    // Configuration: 16 segments for testing (can be adjusted)
    // const size_t NUM_DPA_SEGMENTS = 31;
    // const size_t DPA_SEGMENT_SIZE = 32 * 1024 * 1024;  // 100MB each for testing

    const size_t NUM_DPA_SEGMENTS = 31;
    const size_t DPA_SEGMENT_SIZE = 32 * 1024 * 1024;  // 100MB each for testing

    auto dpa_segments = allocate_dpa_segments(config.ctx, DPA_SEGMENT_SIZE, NUM_DPA_SEGMENTS);
    if (dpa_segments.empty()) {
        DOCA_LOG_ERR("Failed to allocate DPA segments");
        return EXIT_FAILURE;
    }

    // Use first segment as global area
    flexio_uintptr_t global_area_addr = reinterpret_cast<flexio_uintptr_t>(dpa_segments[0]);
    DOCA_LOG_INFO("Using first segment as global area at device address: %p", (void*)global_area_addr);

    // Call RPC to set the global area pointer on the device
    DOCA_LOG_INFO("Setting global area pointer via RPC");
    uint64_t rpc_ret_val; // Placeholder for return value
    Assert(flexio_process_call(config.ctx->get_process(), &dpastore_device_set_global_area, &rpc_ret_val, global_area_addr) == FLEXIO_STATUS_SUCCESS);
    DOCA_LOG_INFO("Global area pointer set successfully");

    // Call RPC to set the host global area window info on the device
    DOCA_LOG_INFO("Setting host global area window info via RPC");
    struct {
        flexio_uintptr_t host_global_area_addr;
        uint32_t window_id;
        uint32_t mkey;
    } window_info = {
        .host_global_area_addr = (flexio_uintptr_t)host_global_area,
        .window_id = config.ctx->get_window_id(),
        .mkey = host_global_mr->lkey
    };
    flexio_uintptr_t window_info_dev_addr = config.ctx->move_to_dev(window_info);
    Assert(flexio_process_call(config.ctx->get_process(), &dpastore_device_set_host_global_area, &rpc_ret_val, window_info_dev_addr) == FLEXIO_STATUS_SUCCESS);
    DOCA_LOG_INFO("Host global area window info set successfully");

    uint32_t current_thread = 0;

    // Create and initialize the stitcher threads on DPA
    DOCA_LOG_INFO("Creating %u stitcher event handlers for DPA->host stitch processing",
                  DPASTORE_STITCHER_THREAD_COUNT);
    for (uint32_t queue = 0; queue < DPASTORE_STITCHER_THREAD_COUNT; ++queue) {
        config.ctx->create_event_handler(dpastore_device_stitcher_thread, 0,
                                         FLEX::Context::EventHandlerAffinity::Strict);
    }

    std::vector<uint32_t> stitcher_activation_ids;
    stitcher_activation_ids.reserve(DPASTORE_STITCHER_THREAD_COUNT);
    for (uint32_t queue = 0; queue < DPASTORE_STITCHER_THREAD_COUNT; ++queue) {
        stitcher_activation_ids.push_back(config.ctx->event_handler_get_activation_id(queue));
    }

    for (uint32_t queue = 0; queue < DPASTORE_STITCHER_THREAD_COUNT; ++queue) {
        uint32_t stitcher_thread_id = current_thread++;
        uint32_t activation_id = stitcher_activation_ids[queue];

        // Run the stitcher event handler with the queue id as device data
        config.ctx->event_handler_run(queue, queue);

        uint64_t packed_data = (static_cast<uint64_t>(queue) << 32) | static_cast<uint64_t>(activation_id);
        DOCA_LOG_INFO("Initializing stitcher %u on DPA (activation ID: %u, thread ID: %u)",
                      queue, activation_id, stitcher_thread_id);
        Assert(flexio_process_call(config.ctx->get_process(),
                                   &dpastore_device_init_stitcher,
                                   &rpc_ret_val,
                                   packed_data) == FLEXIO_STATUS_SUCCESS);
    }

    DOCA_LOG_INFO("All stitcher threads initialized and activated successfully");

    // Give the stitcher threads time to fully start up
    usleep(100000);  // 100ms delay
    DOCA_LOG_INFO("Stitcher threads ready to process commands");

    // Create and initialize host tree using buffers from host global area
    DOCA_LOG_INFO("=== Initializing Host Tree ===");

    // Use the tree node pools from host global area
    void* leaf_buffer = host_global_area->tree_leaf_pool;
    void* inner_buffer = host_global_area->tree_inner_pool;

    DOCA_LOG_INFO("Tree node pools allocated from host global area");

    // Create the host learned tree with host global area and DPA segments
    // NOTE: The stitcher thread must be running before we create the tree,
    // as bulk loading will send stitch commands that need to be processed
    DOCA_LOG_INFO("Creating host learned tree (stitcher thread is already running)");
    HostLearnedTree* host_tree = new HostLearnedTree(leaf_buffer, HOST_TREE_LEAF_POOL_SIZE,
                                                     inner_buffer, HOST_TREE_INNER_POOL_SIZE,
                                                     host_global_area, dpa_segments, DPA_SEGMENT_SIZE);

    std::vector<KVPair> test_data;
    bool using_trace_data = false;
    auto data_gen_start = std::chrono::high_resolution_clock::now();
    std::chrono::microseconds data_gen_duration{0};

#if DPASTORE_BULK_LOAD_FROM_TRACE
    DOCA_LOG_INFO("Loading bulk data from trace: %s", DPASTORE_BULK_LOAD_TRACE_PATH);
    if (dpastore_load_trace_for_bulk(test_data, DPASTORE_BULK_LOAD_TRACE_PATH)) {
        auto trace_end = std::chrono::high_resolution_clock::now();
        data_gen_duration = std::chrono::duration_cast<std::chrono::microseconds>(trace_end - data_gen_start);
        using_trace_data = true;
        DOCA_LOG_INFO("Loaded %zu unique INSERTs from trace in %.3f ms",
                      test_data.size(), data_gen_duration.count() / 1000.0);
    } else {
        DOCA_LOG_WARN("Falling back to synthetic bulk-load dataset");
        test_data.clear();
    }
#endif

    if (!using_trace_data) {
        DOCA_LOG_INFO("Generating test data for bulk loading");
        dpastore::ZipfWorkloadConfig workload_cfg = dpastore::load_zipf_config_from_defines();
        workload_cfg.total_keys = DPASTORE_BULK_LOAD_NUM_KEYS;
        workload_cfg.min_key = DPASTORE_BULK_LOAD_MIN_KEY;
        workload_cfg.max_key = DPASTORE_BULK_LOAD_MAX_KEY;

        const uint64_t partition_span = dpastore::compute_partition_span(workload_cfg);
        const uint64_t partition_target = dpastore::compute_partition_target(workload_cfg);
        if (workload_cfg.table_size == 0 || workload_cfg.table_size > dpastore::kZipfTableDefaultEntries) {
            workload_cfg.table_size = static_cast<uint32_t>(std::min<uint64_t>(partition_target,
                static_cast<uint64_t>(dpastore::kZipfTableDefaultEntries)));
            if (workload_cfg.table_size == 0) {
                workload_cfg.table_size = 1;
            }
        }

        test_data.reserve(workload_cfg.total_keys);

        for (uint32_t thread_id = 0; thread_id < DPASTORE_READER_THREAD_COUNT && test_data.size() < workload_cfg.total_keys; ++thread_id) {
            const uint64_t remaining = workload_cfg.total_keys - test_data.size();
            if (remaining == 0) {
                break;
            }
            const uint64_t base_key = dpastore::partition_base_key(workload_cfg, thread_id, partition_span);
            const uint64_t limit_key = dpastore::partition_limit_key(workload_cfg, thread_id, partition_span);
            const uint64_t span = limit_key - base_key;
            uint64_t keys_needed = std::min<uint64_t>(partition_target, remaining);
            if (keys_needed > span) {
                keys_needed = span;
            }
            if (keys_needed == 0) {
                continue;
            }

            UniformPlan plan = dpastore::make_uniform_plan(base_key, span, keys_needed,
                                                           dpastore::mix_partition_seed(workload_cfg.seed, thread_id));

            std::mt19937_64 value_rng(dpastore::mix_partition_seed(workload_cfg.seed ^ 0x5bd1e995ULL, thread_id));
            for (uint64_t rank = 0; rank < keys_needed; ++rank) {
                uint64_t key = uniform_key_for_rank(&plan, rank);
                if (test_data.size() >= workload_cfg.total_keys) {
                    break;
                }
                test_data.emplace_back(key, value_rng());
            }
        }

        std::sort(test_data.begin(), test_data.end(), [](const KVPair &lhs, const KVPair &rhs) {
            return lhs.key < rhs.key;
        });
        test_data.erase(std::unique(test_data.begin(), test_data.end(), [](const KVPair &lhs, const KVPair &rhs) {
            return lhs.key == rhs.key;
        }), test_data.end());

        if (test_data.size() > workload_cfg.total_keys) {
            test_data.resize(workload_cfg.total_keys);
        }
        if (test_data.size() < workload_cfg.total_keys) {
            DOCA_LOG_WARN("Generated %zu keys (< target %llu); adjust total_keys or key range",
                          test_data.size(),
                          static_cast<unsigned long long>(workload_cfg.total_keys));
        }

        auto data_gen_end = std::chrono::high_resolution_clock::now();
        data_gen_duration = std::chrono::duration_cast<std::chrono::microseconds>(data_gen_end - data_gen_start);

        DOCA_LOG_INFO("Zipf workload config: %s (partition_span=%llu, target_per_thread=%llu)",
                      dpastore::describe(workload_cfg).c_str(),
                      static_cast<unsigned long long>(partition_span),
                      static_cast<unsigned long long>(partition_target));
        DOCA_LOG_INFO("Generated %zu key-value pairs in %.3f ms",
                      test_data.size(), data_gen_duration.count() / 1000.0);
    }

    TestDataGenerator::print_stats(test_data);

    // Bulk load the tree - measure time
    DOCA_LOG_INFO("Bulk loading tree with %zu entries", test_data.size());
    auto bulk_load_start = std::chrono::high_resolution_clock::now();
    host_tree->bulk_load(test_data);
    auto bulk_load_end = std::chrono::high_resolution_clock::now();

    auto bulk_load_duration = std::chrono::duration_cast<std::chrono::microseconds>(bulk_load_end - bulk_load_start);
    double entries_per_sec = (test_data.size() * 1000000.0) / bulk_load_duration.count();
    double mega_ops = entries_per_sec / 1000000.0;
    DOCA_LOG_INFO("Tree bulk load complete in %.3f ms (%.0f entries/sec, %.2f Mops)",
                  bulk_load_duration.count() / 1000.0, entries_per_sec, mega_ops);

    // Send the complete learned tree to DPA via stitch commands - measure time
    DOCA_LOG_INFO("Sending complete learned tree to DPA via stitch commands...");
    auto tree_send_start = std::chrono::high_resolution_clock::now();
    host_tree->send_complete_tree_to_dpa();
    auto tree_send_end = std::chrono::high_resolution_clock::now();

    auto tree_send_duration = std::chrono::duration_cast<std::chrono::microseconds>(tree_send_end - tree_send_start);
    DOCA_LOG_INFO("Tree transfer to DPA initiated - took %.3f ms",
                  tree_send_duration.count() / 1000.0);

    // Print summary of initialization times
    DOCA_LOG_INFO("=== Initialization Performance Summary ===");
    DOCA_LOG_INFO("  Data generation: %.3f ms", data_gen_duration.count() / 1000.0);
    DOCA_LOG_INFO("  Bulk load:       %.3f ms (%.0f entries/sec, %.2f Mops)",
                  bulk_load_duration.count() / 1000.0,
                  entries_per_sec, mega_ops);
    DOCA_LOG_INFO("  Tree send to DPA: %.3f ms", tree_send_duration.count() / 1000.0);
    DOCA_LOG_INFO("  Total time:       %.3f ms",
                  (data_gen_duration.count() + bulk_load_duration.count() + tree_send_duration.count()) / 1000.0);

    // Test lookups to verify the tree is working
    // Pick keys from different parts of the sorted dataset to ensure they exist
    DOCA_LOG_INFO("Testing lookups...");

    size_t test_indices[] = {0, test_data.size() / 4, test_data.size() / 2,
                             3 * test_data.size() / 4, test_data.size() - 1};

    for (size_t idx : test_indices) {
        if (idx < test_data.size()) {
            uint64_t value;
            uint64_t test_key = test_data[idx].key;
            uint64_t expected_value = test_data[idx].value;

            bool found = host_tree->find(test_key, value);
            if (found && value == expected_value) {
                DOCA_LOG_INFO("  Key %lu (index %zu): FOUND, value = %lu ✓",
                             test_key, idx, value);
            } else if (found) {
                DOCA_LOG_WARN("  Key %lu (index %zu): FOUND but wrong value %lu (expected %lu)",
                             test_key, idx, value, expected_value);
            } else {
                DOCA_LOG_WARN("  Key %lu (index %zu): NOT FOUND (expected value %lu)",
                             test_key, idx, expected_value);
            }
        }
    }

    // Also test a key that definitely doesn't exist
    uint64_t non_existent_key = UINT64_MAX - 1;
    uint64_t dummy_value;
    if (!host_tree->find(non_existent_key, dummy_value)) {
        DOCA_LOG_INFO("  Key %lu: correctly NOT FOUND (non-existent key) ✓", non_existent_key);
    } else {
        DOCA_LOG_WARN("  Key %lu: unexpectedly FOUND (should not exist)", non_existent_key);
    }

//#define DEEP_TESTING
#ifdef DEEP_TESTING

    DOCA_LOG_INFO("Running deep lookup test for all %zu keys", test_data.size());
    size_t keys_found = 0;
    size_t keys_not_found = 0;

    for (size_t idx = 0; idx < test_data.size(); ++idx) {
        const auto &kv = test_data[idx];
        uint64_t value = 0;
        if (host_tree->find(kv.key, value)) {
            keys_found++;
            if (value != kv.value) {
                DOCA_LOG_WARN("  Key %lu (index %zu): value mismatch (expected %lu, got %lu)", kv.key, idx, kv.value, value);
            }
        } else {
            keys_not_found++;
            DOCA_LOG_WARN("  Key %lu (index %zu): NOT FOUND", kv.key, idx);
        }
    }

    DOCA_LOG_INFO("Deep lookup test complete: found %zu keys, not found %zu keys", keys_found, keys_not_found);

#endif

    // usleep(100000); // Sleep 100ms
    // return EXIT_SUCCESS;

    DOCA_LOG_INFO("Setting up flow tables and rules");
    FLEX::flow_matcher matcher{};

    DOCA_LOG_INFO("Creating RX domain and flow table");
    auto *global_rx_dr = new FLEX::DR(config.ctx, MLX5DV_DR_DOMAIN_TYPE_NIC_RX);
    matcher.set_dst_mac_mask();
    matcher.set_dst_udp_port_mask();
    config.global_rx_flow_table = global_rx_dr->create_flow_table(0, 0, &matcher);

    matcher.clear();

    DOCA_LOG_INFO("Creating TX domain and flow tables");
    config.global_tx_dr = new FLEX::DR(config.ctx, MLX5DV_DR_DOMAIN_TYPE_FDB);
    if (!config.global_tx_dr) {
        DOCA_LOG_ERR("Failed to create global_tx_dr");
        return EXIT_FAILURE;
    }
    matcher.set_src_mac_mask();
    config.global_tx_flow_root_table = config.global_tx_dr->create_flow_table(0, 0, &matcher);
    if (!config.global_tx_flow_root_table) {
        DOCA_LOG_ERR("Failed to create global_tx_flow_root_table");
        return EXIT_FAILURE;
    }
    config.global_tx_flow_table = config.global_tx_dr->create_flow_table(1, 0, &matcher);
    if (!config.global_tx_flow_table) {
        DOCA_LOG_ERR("Failed to create global_tx_flow_table");
        return EXIT_FAILURE;
    }

    matcher.clear();

    // Reader Threads
    DOCA_LOG_INFO("Initializing %d reader threads", DPASTORE_READER_THREAD_COUNT);
    for (uint32_t i = 0; i < DPASTORE_READER_THREAD_COUNT; i++) {
        init_worker_thread(
            &config.dpastore_configs_reader[i],
            &config,
            matcher,
            i,
            current_thread,
            &dpastore_device_event_handler_dpastore_reader,
            &dpastore_device_init_dpastore_reader,
            DPASTORE_UDPPORT_READER
        );
        current_thread++;
    }

    // Updater Threads
    DOCA_LOG_INFO("Initializing %d updater threads", DPASTORE_UPDATER_THREAD_COUNT);
    for (uint32_t i = 0; i < DPASTORE_UPDATER_THREAD_COUNT; i++) {
        init_worker_thread(
            &config.dpastore_configs_updater[i],
            &config,
            matcher,
            i,
            current_thread,
            &dpastore_device_event_handler_dpastore_updater,
            &dpastore_device_init_dpastore_updater,
            DPASTORE_UDPPORT_UPDATER
        );
        current_thread++;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    DOCA_LOG_INFO("=== DPA-Store X86 Server Started Successfully ===");
    DOCA_LOG_INFO("Memory Configuration Summary:");
    DOCA_LOG_INFO("  Host Global Area: %zu MB", sizeof(host_global_area_t) / (1024 * 1024));
    DOCA_LOG_INFO("    - Host tree leaf pool: %zu MB", HOST_TREE_LEAF_POOL_SIZE / (1024 * 1024));
    DOCA_LOG_INFO("    - Host tree inner pool: %zu MB", HOST_TREE_INNER_POOL_SIZE / (1024 * 1024));
    DOCA_LOG_INFO("    - Patch queues: %d x %zu KB", NUM_PATCH_QUEUES, PATCH_QUEUE_SIZE / 1024);
    DOCA_LOG_INFO("    - Stitch queue: %zu KB", STITCH_QUEUE_SIZE / 1024);
    DOCA_LOG_INFO("  DPA Device Memory: %zu segments x %zu MB = %.2f MB total",
                  dpa_segments.size(), DPA_SEGMENT_SIZE / (1024 * 1024),
                  (dpa_segments.size() * DPA_SEGMENT_SIZE) / (1024.0 * 1024.0));
    DOCA_LOG_INFO("    - First segment used as global area: %zu MB", DPA_SEGMENT_SIZE / (1024 * 1024));
    DOCA_LOG_INFO("    - Remaining segments contributing to pools: %zu segments x %zu MB",
                  dpa_segments.size() - 1, DPA_SEGMENT_SIZE / (1024 * 1024));
    DOCA_LOG_INFO("System Configuration:");
    DOCA_LOG_INFO("  - Reader threads: %d", DPASTORE_READER_THREAD_COUNT);
    DOCA_LOG_INFO("  - Updater threads: %d", DPASTORE_UPDATER_THREAD_COUNT);
    DOCA_LOG_INFO("  - Device: %s", config.device_name.c_str());
    print_tree_info(host_tree);
    DOCA_LOG_INFO("Press 't' for tree info, 'l' for partition layout, 's' for DPA stats, or Ctrl+C to terminate");

    /* Start the patcher workers to scan DPA->host queue */
    DOCA_LOG_INFO("Starting patcher workers to monitor DPA->host queues");
    Patcher patcher(host_global_area, host_tree, host_tree->get_dpa_allocator());
    patcher.start();

    // Initialize stats timing
    auto last_stats_time = std::chrono::steady_clock::now();
    auto last_auto_stats_time = std::chrono::steady_clock::now();

    // Set terminal to non-blocking mode for key press detection
    struct termios old_tio, new_tio;
    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;
    new_tio.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    while (!force_quit) {
        // Check for automatic stats printing (every 3 seconds)
        auto now = std::chrono::steady_clock::now();
        auto elapsed_since_auto = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_auto_stats_time);

        if (elapsed_since_auto.count() >= 3000) {  // 3 seconds
            uint64_t total_packets = 0;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_stats_time);
            if (flexio_process_call(config.ctx->get_process(), &print_and_reset_periodic_throughput,
                                   &total_packets, elapsed.count()) != FLEXIO_STATUS_SUCCESS) {
                DOCA_LOG_WARN("Failed to call device stats printing function");
            }
            last_stats_time = now;
            last_auto_stats_time = now;
        }

        // Check for manual key presses
        char ch;
        if (read(STDIN_FILENO, &ch, 1) == 1) {
            switch (ch) {
                case 't':
                case 'T':
                    print_tree_info(host_tree);
                    patcher.print_stats();
                    break;
                case 'l':
                case 'L':
                    host_tree->log_partition_layout();
                    break;
                case 's':
                case 'S':
                    // Manual stats request
                    print_dpa_stats(config, print_and_reset_periodic_throughput, last_stats_time);
                    break;
                case 'q':
                case 'Q':
                    force_quit = true;
                    break;
            }
        }
        usleep(100000); // Sleep 100ms
    }

    // Restore terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);

    // Print throughput statistics on shutdown
    DOCA_LOG_INFO("=== DPA-Store Shutdown Statistics ===");

    // Stop the patcher workers
    DOCA_LOG_INFO("Stopping patcher workers...");
    patcher.stop();
    DOCA_LOG_INFO("Patcher processed %lu commands, %lu failed",
                  patcher.get_commands_processed(), patcher.get_commands_failed());

    // Call device-side printing function for final stats
    auto final_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - last_stats_time);
    if (flexio_process_call(config.ctx->get_process(), &print_and_reset_periodic_throughput, &rpc_ret_val, final_elapsed.count()) == FLEXIO_STATUS_SUCCESS) {
        DOCA_LOG_INFO("Device statistics printed above");
    } else {
        DOCA_LOG_WARN("Failed to call device stats printing function");
    }

    DOCA_LOG_INFO("=== Shutdown Complete ===");

    /* Cleanup before exit */
    delete host_tree;  // Tree destructor will handle pool allocator cleanup
    host_global_area_destroy(host_global_area, host_global_mr);

    return EXIT_SUCCESS;
}
