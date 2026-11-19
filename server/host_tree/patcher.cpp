#include <doca_log.h>
DOCA_LOG_REGISTER(HOST_TREE_PATCHER);

#include "patcher.hpp"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "../shared/stitch_queue_guard.hpp"

namespace {
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint64_t compute_patch_buffer_hash(const command_patch_buffer_full_t* patch_cmd) {
    if (!patch_cmd) {
        return 0;
    }

    uint64_t hash = kFnvOffsetBasis;
    const uint32_t count = std::min<uint32_t>(patch_cmd->buffer_count, LEARNED_INSERT_BUFFER_SIZE);

    auto mix = [](uint64_t& h, uint64_t value) {
        h ^= value;
        h *= kFnvPrime;
    };

    mix(hash, static_cast<uint64_t>(count));
    for (uint32_t i = 0; i < count; ++i) {
        mix(hash, patch_cmd->buffer_entries[i].key);
        mix(hash, patch_cmd->buffer_entries[i].value);
    }
    return hash;
}

struct PatchBufferSummary {
    uint64_t first_key = 0;
    uint64_t last_key = 0;
};

PatchBufferSummary summarize_patch_buffer(const command_patch_buffer_full_t* patch_cmd) {
    PatchBufferSummary summary{};
    if (!patch_cmd || patch_cmd->buffer_count == 0) {
        return summary;
    }

    const uint32_t count = std::min<uint32_t>(patch_cmd->buffer_count, LEARNED_INSERT_BUFFER_SIZE);
    summary.first_key = patch_cmd->buffer_entries[0].key;
    summary.last_key = patch_cmd->buffer_entries[count - 1].key;
    return summary;
}
} // namespace



Patcher::Patcher(host_global_area_t *host_global_area, HostLearnedTree *tree, DPATreeAllocator *allocator) 
    : hga(host_global_area),
      host_tree(tree),
      dpa_allocator(allocator),
      running(false),
      commands_processed(0),
      commands_failed(0),
      stitches_sent(0) {
    if (!hga) {
        DOCA_LOG_ERR("Invalid host global area provided to Patcher");
        throw std::invalid_argument("host_global_area cannot be null");
    }
    if (!host_tree) {
        DOCA_LOG_ERR("Invalid host tree provided to Patcher");
        throw std::invalid_argument("host_tree cannot be null");
    }
    if (!dpa_allocator) {
        DOCA_LOG_ERR("Invalid DPA allocator provided to Patcher");
        throw std::invalid_argument("dpa_allocator cannot be null");
    }
}

Patcher::~Patcher() {
    stop();
}

void Patcher::start() {
    if (running.load()) {
        DOCA_LOG_WARN("Patcher threads already running");
        return;
    }

    const uint32_t patcher_count = static_cast<uint32_t>(DPASTORE_PATCHER_THREAD_COUNT);
    if (patcher_count == 0) {
        DOCA_LOG_ERR("DPASTORE_PATCHER_THREAD_COUNT is zero; no patcher threads launched");
        return;
    }

    running = true;
    patcher_threads.clear();
    patcher_threads.reserve(patcher_count);

    const uint32_t total_patch_queues = static_cast<uint32_t>(NUM_PATCH_QUEUES);
    const uint32_t queues_per_patcher = patcher_count ?
        (total_patch_queues + patcher_count - 1) / patcher_count : 0;

    for (uint32_t id = 0; id < patcher_count; ++id) {
        uint32_t begin = queues_per_patcher ? id * queues_per_patcher : 0;
        uint32_t end = queues_per_patcher ? std::min(begin + queues_per_patcher, total_patch_queues)
                                          : total_patch_queues;
        patcher_threads.emplace_back(&Patcher::patcher_loop, this, id, begin, end);
    }

    DOCA_LOG_INFO("Patcher threads started (%u workers)", patcher_count);
}

void Patcher::stop() {
    if (!running.exchange(false)) {
        return;
    }

    DOCA_LOG_INFO("Stopping patcher threads...");

    for (auto &worker : patcher_threads) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    patcher_threads.clear();

    DOCA_LOG_INFO("Patcher threads stopped. Commands processed: %lu, failed: %lu, stitches sent: %lu",
                  commands_processed.load(), commands_failed.load(), stitches_sent.load());
}

void Patcher::patcher_loop(uint32_t patcher_id, uint32_t queue_begin, uint32_t queue_end) {

    DOCA_LOG_INFO("Patcher worker %u monitoring patch queues [%u, %u)",
                  patcher_id, queue_begin, queue_end);

    if (queue_begin >= queue_end) {
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        DOCA_LOG_INFO("Patcher worker %u exiting (no queues assigned)", patcher_id);
        return;
    }

    uint8_t cmd_buffer[512];  // Buffer for receiving commands
    uint32_t actual_size;

    while (running.load()) {
        bool received_any = false;

        for (uint32_t queue = queue_begin; queue < queue_end; ++queue) {
            if (host_receive_patch(hga, queue, cmd_buffer, sizeof(cmd_buffer), &actual_size)) {
                //host_tree->root_lock_.lock();
                process_command(cmd_buffer, actual_size, patcher_id);
                //host_tree->root_lock_.unlock();
                commands_processed++;
                received_any = true;

                // if (commands_processed % 100 == 0) {
                //     DOCA_LOG_INFO("Patcher worker %u processed %lu patch commands",
                //                   patcher_id, commands_processed.load());
                // }
            }
        }

        if (!received_any) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    DOCA_LOG_INFO("Patcher worker %u exiting", patcher_id);
}

void Patcher::print_stats() const {
    DOCA_LOG_INFO("=== Patcher Statistics ===");
    DOCA_LOG_INFO("  - Commands processed: %lu", commands_processed.load());
    DOCA_LOG_INFO("  - Commands failed: %lu", commands_failed.load());
    DOCA_LOG_INFO("  - Test stitches sent: %lu", stitches_sent.load());
    DOCA_LOG_INFO("  - Status: %s", is_running() ? "Running" : "Stopped");

    // Print HGA queue statistics
    DOCA_LOG_INFO("=== Host Global Area Statistics ===");
}

void Patcher::process_command(const uint8_t *cmd_buffer, uint32_t cmd_size, size_t thread_id) {
#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (cmd_size < sizeof(command_generic_t)) {
        DOCA_LOG_WARN("Received command too small: %u bytes", cmd_size);
        commands_failed++;
        return;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER
    
    const command_generic_t *generic_cmd = (const command_generic_t *)cmd_buffer;
    
#if DEBUG_INSERTS_MULTI_PATCHER == 1

    // Validate command size matches header
    if (generic_cmd->size != cmd_size) {
        DOCA_LOG_WARN("Command size mismatch: header=%u, actual=%u", 
                     generic_cmd->size, cmd_size);
        commands_failed++;
        return;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER
    
    // Process based on command type
    switch (generic_cmd->type) {
#if DEBUG_INSERTS_MULTI_PATCHER == 1
        case CMD_TYPE_TEST: {
            if (cmd_size != sizeof(command_test_t)) {
                DOCA_LOG_WARN("Invalid test command size: %u", cmd_size);
                commands_failed++;
                break;
            }
            
            const command_test_t *test_cmd = (const command_test_t *)cmd_buffer;
            DOCA_LOG_INFO("Patcher received test command: %.56s", test_cmd->data);
            break;
        }
#endif // DEBUG_INSERTS_MULTI_PATCHER
        
        case CMD_TYPE_PATCH_BUFFER_FULL: {
#if DEBUG_INSERTS_MULTI_PATCHER == 1
            if (cmd_size != sizeof(command_patch_buffer_full_t)) {
                DOCA_LOG_WARN("Invalid buffer full patch command size: %u", cmd_size);
                commands_failed++;
                break;
            }
#endif // DEBUG_INSERTS_MULTI_PATCHER
            
            const command_patch_buffer_full_t *patch_cmd = (const command_patch_buffer_full_t *)cmd_buffer;
            // DOCA_LOG_INFO("Patcher received buffer full patch from thread %u for leaf %p with %u entries", 
            //              patch_cmd->thread_id, patch_cmd->dpa_leaf_ptr, patch_cmd->buffer_count);
            
            process_buffer_full_patch(patch_cmd, thread_id);
            break;
        }
        
        default:
            DOCA_LOG_WARN("Unknown command type: %u", generic_cmd->type);
            commands_failed++;
            break;
    }
}

void Patcher::send_test_stitch(uint32_t thread_id) {
    // Create a test stitch command
    command_stitch_t test_stitch;
    test_stitch.type = CMD_TYPE_STITCH;
    test_stitch.size = sizeof(command_stitch_t);
    
    // Format test message into data field
    snprintf((char*)test_stitch.data, sizeof(test_stitch.data), 
             "Test stitch response to patch from thread %u", thread_id);
    
    // Send the stitch command to DPA via the stitch queue
    const uint32_t queue_id = NUM_STITCH_QUEUES ? (thread_id % NUM_STITCH_QUEUES) : 0;
    if (host_send_stitch_thread_safe(hga, queue_id, &test_stitch, sizeof(test_stitch))) {
        stitches_sent++;
        DOCA_LOG_INFO("Sent test stitch command to DPA (total sent: %lu)", 
                     stitches_sent.load());
    } else {
        DOCA_LOG_WARN("Failed to send test stitch command - queue may be full");
    }
}

void Patcher::process_buffer_full_patch(const command_patch_buffer_full_t *patch_cmd, size_t thread_id) {
#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (!patch_cmd || !host_tree) {
        DOCA_LOG_ERR("Invalid parameters for process_buffer_full_patch");
        commands_failed++;
        return;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER
    
    // Step 1: Get host leaf node pointer directly from patch command
    HostLearnedLeafNode* host_leaf = static_cast<HostLearnedLeafNode*>(patch_cmd->host_leaf_ptr);
#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (!host_leaf) {
        DOCA_LOG_ERR("Invalid host leaf pointer in patch command");
        commands_failed++;
        return;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER
    
    // DOCA_LOG_INFO("Using host leaf node at %p for DPA leaf %p", host_leaf, patch_cmd->dpa_leaf_ptr);
    
    // Log the buffer contents
    // DOCA_LOG_INFO("Processing buffer full patch with %u entries:", patch_cmd->buffer_count);
    // for (uint32_t i = 0; i < patch_cmd->buffer_count; i++) {
    //     DOCA_LOG_INFO("  Entry %u: key=%lu, value=%lu", i,
    //                   patch_cmd->buffer_entries[i].key,
    //                   patch_cmd->buffer_entries[i].value);
    // }
    
    // DOCA_LOG_INFO("Patcher invoking update_leaf for leaf %p (%u entries)", host_leaf, patch_cmd->buffer_count);
    std::vector<uint64_t> keys(patch_cmd->buffer_count);
    std::vector<uint64_t> values(patch_cmd->buffer_count);
    for (uint32_t i = 0; i < patch_cmd->buffer_count; ++i) {
        keys[i] = patch_cmd->buffer_entries[i].key;
        values[i] = patch_cmd->buffer_entries[i].value;
    }

    const uint64_t* keys_ptr = patch_cmd->buffer_count ? keys.data() : nullptr;
    const uint64_t* values_ptr = patch_cmd->buffer_count ? values.data() : nullptr;

    //#define PATCH_RECEIVE_DEBUGGING
    #ifdef PATCH_RECEIVE_DEBUGGING
    if (patch_cmd->buffer_count > 0) {
        uint64_t min_key = patch_cmd->buffer_entries[0].key;
        for (uint32_t i = 1; i < patch_cmd->buffer_count; ++i) {
            uint64_t key = patch_cmd->buffer_entries[i].key;
            if (key < min_key) {
                min_key = key;
            }
        }

        if (min_key < host_leaf->first_key) {
            DOCA_LOG_INFO("PATCH_RECEIVE_DEBUG leaf=%p dpa_leaf=%p buffer_count=%u dpa_first_key=%lu host_first_key=%lu buffer_min_key=%lu",
                          host_leaf,
                          patch_cmd->dpa_leaf_ptr,
                          patch_cmd->buffer_count,
                          patch_cmd->first_key,
                          host_leaf->first_key,
                          min_key);
            //host_tree->send_unflag_stitch(host_leaf);
            //return; // TODO: test this edge case.
        }
    }

    {
        PatchRecord existing_record{};
        bool is_duplicate = false;

        const PatchKey key{patch_cmd->dpa_leaf_ptr, patch_cmd->first_key};
        const uint64_t buffer_hash = compute_patch_buffer_hash(patch_cmd);
        const PatchBufferSummary summary = summarize_patch_buffer(patch_cmd);
        const PatchRecord new_record{host_leaf,
                                     patch_cmd->thread_id,
                                     patch_cmd->buffer_count,
                                     patch_cmd->periodic_count,
                                     buffer_hash,
                                     summary.first_key,
                                     summary.last_key,
                                     patch_cmd->generation,
                                     patch_cmd->insert_count_total};

        {
            std::lock_guard<std::mutex> lock(patch_registry_mutex);
            auto [it, inserted] = patch_registry.emplace(key, new_record);
            if (!inserted) {
                existing_record = it->second;
                it->second = new_record;
                is_duplicate = true;
            }
        }

        if (is_duplicate) {
            DOCA_LOG_WARN("Duplicate buffer-full patch detected dpa_leaf=%p first_key=%lu \nexisting_host_leaf=%p existing_thread=%u existing_buffer_count=%u existing_periodic=%u existing_hash=%lu existing_buffer_first=%lu existing_buffer_last=%lu \nexisting_generation=%u existing_insert_total_count=%u \nnew_host_leaf=%p new_thread=%u new_buffer_count=%u new_periodic=%u new_hash=%lu new_buffer_first=%lu new_buffer_last=%lu \nnew_generation=%u new_insert_total_count=%u",
                          patch_cmd->dpa_leaf_ptr,
                          patch_cmd->first_key,
                          existing_record.host_leaf,
                          existing_record.thread_id,
                          existing_record.buffer_count,
                          existing_record.periodic_count,
                          static_cast<unsigned long>(existing_record.buffer_hash),
                          static_cast<unsigned long>(existing_record.buffer_first_key),
                          static_cast<unsigned long>(existing_record.buffer_last_key),
                          existing_record.generation,
                          existing_record.insert_total_count,
                          host_leaf,
                          patch_cmd->thread_id,
                          patch_cmd->buffer_count,
                          patch_cmd->periodic_count,
                          static_cast<unsigned long>(buffer_hash),
                          static_cast<unsigned long>(summary.first_key),
                          static_cast<unsigned long>(summary.last_key),
                          patch_cmd->generation,
                          patch_cmd->insert_count_total);
        }
    }

    #endif // PATCH_RECEIVE_DEBUGGING

    if (!host_tree->update_leaf(host_leaf, keys_ptr, values_ptr, patch_cmd->buffer_count, thread_id)) {
        //DOCA_LOG_ERR("update_leaf failed for host leaf %p", host_leaf);
        commands_failed++;
        return;
    }

    // DOCA_LOG_INFO("Patcher finished update_leaf for leaf %p", host_leaf);
    // DOCA_LOG_INFO("Buffer full patch handling complete (host leaf %p, DPA leaf %p)",
    //               host_leaf,
    //               patch_cmd->dpa_leaf_ptr);

    // TODO: once RCU reclamation lands, track deferred frees and metrics here —
    //       update_leaf already emitted the necessary stitch commands.
}
