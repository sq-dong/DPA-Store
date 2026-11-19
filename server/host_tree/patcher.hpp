#ifndef PATCHER_HPP
#define PATCHER_HPP

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "../host_global_area_mgmt.h"
#include "../shared/host_global_area.h"
#include "../shared/commands.h"
#include "host_learned_tree.hpp"
#include "dpatree_allocator.hpp"

/* Patcher thread class that scans DPA->host queues and patches host tree
 * The patcher runs continuously, pulling commands from the DPA and applying
 * them to the host-side tree structure */
class Patcher {
private:
    host_global_area_t *hga;           /* Host global area with ring buffers */
    HostLearnedTree *host_tree;        /* Host learned tree to apply patches to */
    DPATreeAllocator *dpa_allocator;   /* DPA allocator for address translation */
    std::atomic<bool> running;         /* Flag to control thread lifecycle */
    std::vector<std::thread> patcher_threads;  /* Patcher worker threads */
    
    /* Statistics */
    std::atomic<uint64_t> commands_processed;
    std::atomic<uint64_t> commands_failed;
    
    /* Thread function that runs the patcher loop */
    void patcher_loop(uint32_t patcher_id, uint32_t queue_begin, uint32_t queue_end);
    
    /* Process a single command from the queue */
    void process_command(const uint8_t *cmd_buffer, uint32_t cmd_size, size_t thread_id);
    
    /* Send a test stitch command to DPA */
    void send_test_stitch(uint32_t thread_id);
    
    /* Process buffer full patch from DPA */
    void process_buffer_full_patch(const command_patch_buffer_full_t *patch_cmd, size_t thread_id);
    
    /* Statistics for stitches */
    std::atomic<uint64_t> stitches_sent;

    struct PatchKey {
        void* dpa_leaf;
        uint64_t first_key;
    };

    struct PatchRecord {
        HostLearnedLeafNode* host_leaf;
        uint32_t thread_id;
        uint32_t buffer_count;
        uint32_t periodic_count;
        uint64_t buffer_hash;
        uint64_t buffer_first_key;
        uint64_t buffer_last_key;
        uint32_t generation;
        uint32_t insert_total_count;
    };

    struct PatchKeyHash {
        size_t operator()(const PatchKey& key) const noexcept {
            const size_t ptr_hash = std::hash<void*>{}(key.dpa_leaf);
            const size_t first_key_hash = std::hash<uint64_t>{}(key.first_key);
            constexpr size_t golden = 0x9e3779b9;
            return ptr_hash ^ (first_key_hash + golden + (ptr_hash << 6) + (ptr_hash >> 2));
        }
    };

    struct PatchKeyEqual {
        bool operator()(const PatchKey& lhs, const PatchKey& rhs) const noexcept {
            return lhs.dpa_leaf == rhs.dpa_leaf && lhs.first_key == rhs.first_key;
        }
    };

    std::unordered_map<PatchKey, PatchRecord, PatchKeyHash, PatchKeyEqual> patch_registry;
    std::mutex patch_registry_mutex;
    
public:
    /* Constructor
     * @param host_global_area: Pointer to initialized host global area
     * @param tree: Pointer to host learned tree
     * @param allocator: Pointer to DPA allocator for address translation */
    Patcher(host_global_area_t *host_global_area, HostLearnedTree *tree, DPATreeAllocator *allocator);
    
    /* Destructor - ensures clean shutdown */
    ~Patcher();
    
    /* Start the patcher thread */
    void start();
    
    /* Stop the patcher thread */
    void stop();
    
    /* Check if patcher is running */
    bool is_running() const { return running.load(); }
    
    /* Get statistics */
    uint64_t get_commands_processed() const { return commands_processed.load(); }
    uint64_t get_commands_failed() const { return commands_failed.load(); }
    
    /* Print current statistics */
    void print_stats() const;
};

#endif /* PATCHER_HPP */
