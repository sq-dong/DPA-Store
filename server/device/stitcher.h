#ifndef __DPASTORE_STITCHER_H__
#define __DPASTORE_STITCHER_H__

#include <stddef.h>
#include "wrapper_flexio_device.h"
#include "common/dpa_common.h"
#include "common/dpastore_common_config.h"
#include "../shared/commands.h"
#include "../shared/host_global_area.h"
#include "../shared/server_config.h"
#include "tree_types.h"

// External reference to host global area address from dpastore_device.c
extern flexio_uintptr_t host_global_area_addr;

// Process a stitch command received from the host

static inline void process_stitch_command(command_generic_t *cmd, host_global_area_t *hga, uint64_t stitcher_thread)
{
    
    // Handle different stitch command types
    switch (cmd->type) {
        case CMD_TYPE_STITCH: {
            command_stitch_t *stitch_cmd = (command_stitch_t *)cmd;
            // TODO: Process stitch command - perform structural tree operations
            // For now just log the test data
            LOG_I("DPA: Received legacy stitch command: %s\n", stitch_cmd->data);
            break;
        }

        case CMD_TYPE_STITCH_COPY_LEARNED_LEAF: {
            command_stitch_copy_learned_leaf_t *leaf_cmd = (command_stitch_copy_learned_leaf_t *)cmd;

            // LOG_I("DPA: Stitch copy learned leaf host=%p -> dpa=%p (sorted=%u, first_key=%lu, last_key=%lu)\n",
            //       leaf_cmd->host_ptr,
            //       leaf_cmd->dpa_ptr,
            //       leaf_cmd->sorted_count,
            //       leaf_cmd->first_key,
            //       leaf_cmd->last_key);
            
            // Copy learned leaf node data to DPA memory at pre-allocated address
            DPALearnedLeafNode *dpa_leaf = (DPALearnedLeafNode *)leaf_cmd->dpa_ptr;

            //#define DEBUG_COPY_LEAF
            #ifdef DEBUG_COPY_LEAF

            if (dpa_leaf->first_key == leaf_cmd->first_key) {
                LOG_I("DPA: Stitch copy learned leaf first key match at %p (dpa=%lu host=%lu) (old_slope=%lu new_slope=%lu)\n",
                    dpa_leaf,
                    dpa_leaf->first_key,
                    leaf_cmd->first_key,
                    dpa_leaf->slope_fp,
                    leaf_cmd->slope_fp);
            }

            #endif // DEBUG_COPY_LEAF
            
            // Set metadata fields
            dpa_leaf->type = DPA_LEARNED_LEAF;
            dpa_leaf->slope_fp = leaf_cmd->slope_fp;
            dpa_leaf->intercept_fp = leaf_cmd->intercept_fp;
            dpa_leaf->sorted_count = leaf_cmd->sorted_count;
            dpa_leaf->buffer_high = 0;  // Initially empty buffer
            dpa_leaf->host_ptr = leaf_cmd->host_ptr;  // Set host pointer

            // dpa_leaf->insert_count_total = 0;
            // dpa_leaf->generation++;

            // Compute offset from host global area base to sorted_entries in host leaf
            // The host leaf structure has sorted_entries after the base fields
            // HostLearnedLeafNode layout: base class + slope_fp + intercept_fp + sorted_entries
            // We need the offset from HGA base to the sorted_entries array
            uintptr_t host_base = (uintptr_t)host_global_area_addr;  // Original host address of hga
            // Offset to the leaf node from HGA base
            uintptr_t leaf_offset = (uintptr_t)leaf_cmd->host_ptr - host_base;
            // Offset to sorted_entries within the leaf (after base + slope_fp + intercept_fp)
            // Base class (8 bytes) + slope_fp (8) + intercept_fp (8) = 24 bytes
            size_t sorted_entries_offset_in_leaf = 72;  // TODO: Use proper sizeof/offsetof
            dpa_leaf->host_hga_to_sorted_keys_offset = leaf_offset + sorted_entries_offset_in_leaf;

            
            dpa_leaf->first_key = leaf_cmd->first_key;
            //dpa_leaf->last_key = leaf_cmd->last_key;
            
            // Copy sorted keys and values separately
#ifndef USE_MICRO_LEAVES
            for (uint32_t i = 0; i < leaf_cmd->sorted_count; i++) {
                dpa_leaf->sorted_keys[i] = leaf_cmd->sorted_entries[i].key;
                dpa_leaf->sorted_values[i] = leaf_cmd->sorted_entries[i].value;
            }
#endif

            // LOG_I("DPA: Stitch copy learned leaf applied at %p (buffer_high=%u)\n",
            //       dpa_leaf,
            //       dpa_leaf->buffer_high);

#if DEBUG_INSERTS_DPU_SIDE_STITCHING == 1
            __dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW);
            dpa_leaf->UUID_version = leaf_cmd->UUID_version;
#endif // DEBUG_INSERTS_DPU_SIDE_STITCHING

            dpa_spinlock_init(&dpa_leaf->buffer_lock);  // Initialize spinlock
            dpa_leaf->update_in_progress = 0;  // No update in progress

            break;
        }
        
        case CMD_TYPE_STITCH_COPY_LEARNED_INNER: {
            command_stitch_copy_learned_inner_t *inner_cmd = (command_stitch_copy_learned_inner_t *)cmd;

#if DEBUG_INSERTS_DPU_SIDE_STITCHING == 1
            LOG_I("DPA: Stitch copy learned inner cmd=%p -> dpa=%p (segments=%u, children=%u, min_key=%lu, stitcher_thread=%lu)\n",
                  inner_cmd,
                  inner_cmd->dpa_ptr,
                  inner_cmd->num_segments,
                  inner_cmd->num_children,
                  inner_cmd->min_key,
                  stitcher_thread);
#endif // DEBUG_INSERTS_DPU_SIDE_STITCHING
            
            // Copy learned inner node data to DPA memory at pre-allocated address
            DPALearnedInnerNode *dpa_inner = (DPALearnedInnerNode *)inner_cmd->dpa_ptr;
            
            // Set metadata fields
            dpa_inner->type = DPA_LEARNED_INNER;
            dpa_inner->tree_level = inner_cmd->tree_level;
            dpa_inner->num_segments = inner_cmd->num_segments;
            dpa_inner->num_children = inner_cmd->num_children;
            dpa_inner->min_key = inner_cmd->min_key;

            // Copy first keys to compact array and segments
            for (uint32_t i = 0; i < inner_cmd->num_segments; i++) {
                dpa_inner->segment_first_keys[i] = inner_cmd->segments[i].first_key;
                dpa_inner->segments[i].first_key = inner_cmd->segments[i].first_key;
                dpa_inner->segments[i].slope_fp = inner_cmd->segments[i].slope_fp;
                dpa_inner->segments[i].intercept_fp = inner_cmd->segments[i].intercept_fp;
                dpa_inner->segments[i].first_child_idx = inner_cmd->segments[i].first_child_idx;
                dpa_inner->segments[i].num_children = inner_cmd->segments[i].num_children;
            }

            // Fill unused segment_first_keys entries with UINT64_MAX for optimized search
            for (uint32_t i = inner_cmd->num_segments; i < LEARNED_SEGMENTS_PER_INNER; i++) {
                dpa_inner->segment_first_keys[i] = UINT64_MAX;
            }

            // Copy child first keys and pointers
            for (uint32_t i = 0; i < inner_cmd->num_children; i++) {
                dpa_inner->child_first_keys[i] = inner_cmd->child_first_keys[i];
                dpa_inner->children[i] = inner_cmd->children[i];
            }

            //__dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW); // CHANGED
            __dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW);  // REVERT DID NOT SOLVE IT
            dpa_inner->UUID_version = inner_cmd->UUID_version;
            
            // LOG_I("DPA: Stitch copy learned inner applied at %p\n", dpa_inner);

            break;
        }
        
        case CMD_TYPE_STITCH_SET_EPOCH: {
            command_stitch_set_epoch_t *epoch_cmd = (command_stitch_set_epoch_t *)cmd;
            extern flexio_uintptr_t global_area_addr;
            DPAStoreGlobalArea* global = (DPAStoreGlobalArea*)global_area_addr;

#if DEBUG_INSERTS_DPU_SIDE_STITCHING == 1
            if (!global) {
                LOG_E("DPA: STITCH_SET_EPOCH received without global area (epoch=%lu, thread=%lu)\n",
                      (unsigned long)epoch_cmd->epoch,
                      stitcher_thread);
                break;
            }

            if (stitcher_thread < NUM_STITCH_QUEUES) {
#endif // DEBUG_INSERTS_DPU_SIDE_STITCHING
                uint64_t current = global->stitcher_epochs[stitcher_thread];
                if (epoch_cmd->epoch > current) {
                    global->stitcher_epochs[stitcher_thread] = epoch_cmd->epoch;
                    __dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_RW);
                }
#if DEBUG_INSERTS_DPU_SIDE_STITCHING == 1
            }

            LOG_I("DPA: Processed CMD_TYPE_STITCH_SET_EPOCH");
#endif // DEBUG_INSERTS_DPU_SIDE_STITCHING

            break;
        }
        
        case CMD_TYPE_STITCH_UPDATE_LEARNED_ROOT: {
            command_stitch_update_learned_root_t *root_cmd =
                (command_stitch_update_learned_root_t *)cmd;

            extern flexio_uintptr_t global_area_addr;
            DPAStoreGlobalArea* global = (DPAStoreGlobalArea*)global_area_addr;

            #if STITCHING_RACE_BARRIERS == 1
            if (global && root_cmd->barrier_epoch != 0) {
                while (1) {
                    bool ready = true;
                    // __dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW); CHANGED
                    __dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW); // IS NEEDED
                    for (uint32_t epoch_idx = 0; epoch_idx < NUM_STITCH_QUEUES; ++epoch_idx) {
                        if (global->stitcher_epochs[epoch_idx] < root_cmd->barrier_epoch) {
                            ready = false;
                            break;
                        }
                    }
                    if (ready) {
                        break;
                    }
                }
            }
            #endif //STITCHING_RACE_BARRIERS

            DPALearnedInnerNode *new_root_location = (DPALearnedInnerNode *)root_cmd->new_root;

            #if STITCHING_RACE_BARRIERS == 1

            //if (root_cmd->stitch_into_root) {

                if (root_cmd->expected_UUID_version > new_root_location->UUID_version) {
                    // NEED TO WAIT
#if DEBUG_INSERTS_DPU_SIDE_STITCHING == 1
                    LOG_E("DPA: STITCH_UPDATE_LEARNED_SUBTREE root=%p ROOTUPDATE_UUID_WAIT. new_root_location_UUID=%lu expected_UUID=%lu stitcher_thread=%lu\n",
                        new_root_location,
                        (unsigned long)new_root_location->UUID_version,
                        (unsigned long)root_cmd->expected_UUID_version,
                        stitcher_thread);
#endif // DEBUG_INSERTS_DPU_SIDE_STITCHING

                         __dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW); // CHANGED, WORKS WITHOUT
                        while(root_cmd->expected_UUID_version > new_root_location->UUID_version) {
                            __dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW); // CHANGED
                            if (root_cmd->expected_UUID_version <= new_root_location->UUID_version) {
                                break;
                            }
                            
                        }
#if DEBUG_INSERTS_DPU_SIDE_STITCHING == 1
                    LOG_E("DPA: STITCH_UPDATE_LEARNED_SUBTREE root=%p ROOTUPDATE_UUID_RESOLVED. new_root_location_UUID=%lu expected_UUID=%lu stitcher_thread=%lu\n",
                        new_root_location,
                        (unsigned long)new_root_location->UUID_version,
                        (unsigned long)root_cmd->expected_UUID_version,
                        stitcher_thread);
#endif // DEBUG_INSERTS_DPU_SIDE_STITCHING
                } 

                dpa_spinlock_acquire(&global->tree.root_replace_lock);
                
                if (root_cmd->expected_UUID_version == new_root_location->UUID_version) {
                    // OK
                } else {
                    dpa_spinlock_release(&global->tree.root_replace_lock);
                    break;
                }
            //}

            #endif //STITCHING_RACE_BARRIERS

            global->tree.root = (DPALearnedTreeNode *)root_cmd->new_root;

            #if STITCHING_RACE_BARRIERS == 1

            //if (root_cmd->stitch_into_root) {
                dpa_spinlock_release(&global->tree.root_replace_lock);
            //}

            #endif //STITCHING_RACE_BARRIERS

#if DEBUG_INSERTS_DPU_SIDE_STITCHING == 1
            LOG_I("DPA: Processed STITCH_UPDATE_LEARNED_ROOT - new_root=%p\n", root_cmd->new_root);
#endif // DEBUG_INSERTS_DPU_SIDE_STITCHING

            break;
        }

        case CMD_TYPE_STITCH_UPDATE_LEARNED_SUBTREE: {
            command_stitch_update_learned_subtree_t *conn_cmd =
                (command_stitch_update_learned_subtree_t *)cmd;
            // LOG_I("DPA: Received STITCH_UPDATE_LEARNED_SUBTREE parent=%p child=%p index=%u\n",
            //       conn_cmd->parent_ptr, conn_cmd->new_child_ptr, conn_cmd->child_index);

            DPALearnedInnerNode *parent_inner = (DPALearnedInnerNode *)conn_cmd->parent_ptr;
            uint16_t child_index = conn_cmd->child_index;

            uint8_t root_stitch_race_result = 0;
            #if STITCHING_RACE_BARRIERS == 1

            if (conn_cmd->stitch_into_root) {
                if (conn_cmd->expected_UUID_version != parent_inner->UUID_version) {
                    // NEED TO WAIT
#if DEBUG_INSERTS_DPU_SIDE_STITCHING == 1
                    LOG_E("DPA: STITCH_UPDATE_LEARNED_SUBTREE parent=%p idx=%u UUID_WAIT. stitch_into_root=%u parent_UUID=%lu expected_UUID=%lu stitcher_thread=%lu\n",
                        parent_inner,
                        (unsigned int)child_index,
                        (unsigned int)conn_cmd->stitch_into_root,
                        (unsigned long)parent_inner->UUID_version,
                        (unsigned long)conn_cmd->expected_UUID_version,
                        stitcher_thread);
#endif // DEBUG_INSERTS_DPU_SIDE_STITCHING

                        __dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW); // CHANGED, WORKS WITHOUT
                        while(conn_cmd->expected_UUID_version > parent_inner->UUID_version) {
                            __dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW); // CHANGED
                            if (conn_cmd->expected_UUID_version <= parent_inner->UUID_version) {
                                break;
                            }
                        }
#if DEBUG_INSERTS_DPU_SIDE_STITCHING == 1
                    LOG_E("DPA: STITCH_UPDATE_LEARNED_SUBTREE parent=%p idx=%u UUID_RESOLVED. stitch_into_root=%u parent_UUID=%lu expected_UUID=%lu stitcher_thread=%lu\n",
                        parent_inner,
                        (unsigned int)child_index,
                        (unsigned int)conn_cmd->stitch_into_root,
                        (unsigned long)parent_inner->UUID_version,
                        (unsigned long)conn_cmd->expected_UUID_version,
                        stitcher_thread);
#endif // DEBUG_INSERTS_DPU_SIDE_STITCHING
                } 

                extern flexio_uintptr_t global_area_addr;
                DPAStoreGlobalArea* global = (DPAStoreGlobalArea*)global_area_addr;
                dpa_spinlock_acquire(&global->tree.root_replace_lock);
                //__dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW);
                
                if (conn_cmd->expected_UUID_version == parent_inner->UUID_version) {
                    root_stitch_race_result = 1;
                    // OK
                } else {
                    if (conn_cmd->expected_UUID_version < parent_inner->UUID_version)
                        root_stitch_race_result = 3;
                    if (conn_cmd->expected_UUID_version > parent_inner->UUID_version)
                        root_stitch_race_result = 4;
                    // dpa_spinlock_release(&global->tree.root_replace_lock);
                    // break;
                }
            } 
#if DEBUG_INSERTS_DPU_SIDE_STITCHING == 1
            else {
                if (conn_cmd->expected_UUID_version != parent_inner->UUID_version) {
                    LOG_E("DPA: STITCH_UPDATE_LEARNED_SUBTREE parent=%p idx=%u UUID_WRONG. stitch_into_root=%u parent_UUID=%lu expected_UUID=%lu stitcher_thread=%lu\n",
                        parent_inner,
                        (unsigned int)child_index,
                        (unsigned int)conn_cmd->stitch_into_root,
                        (unsigned long)parent_inner->UUID_version,
                        (unsigned long)conn_cmd->expected_UUID_version,
                        stitcher_thread);                
                }

            }
#endif // DEBUG_INSERTS_DPU_SIDE_STITCHING

            #endif // STITCHING_RACE_BARRIERS == 1


            uint64_t stored_first_key = parent_inner->child_first_keys[child_index];

            #if STITCHING_RACE_BARRIERS == 1
            if (root_stitch_race_result == 0 || root_stitch_race_result == 1) {      
            #endif // STITCHING_RACE_BARRIERS == 1

                parent_inner->children[child_index] = conn_cmd->new_child_ptr;

            #if STITCHING_RACE_BARRIERS == 1
                //__dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW);
            }
            #endif // STITCHING_RACE_BARRIERS == 1

            #if STITCHING_RACE_BARRIERS == 1
            if (conn_cmd->stitch_into_root) {
                extern flexio_uintptr_t global_area_addr;
                DPAStoreGlobalArea* global = (DPAStoreGlobalArea*)global_area_addr;
                dpa_spinlock_release(&global->tree.root_replace_lock);
            }
            #endif // STITCHING_RACE_BARRIERS == 1

            __dpa_thread_fence(__DPA_MEMORY, __DPA_RW, __DPA_RW); // CHANGED

#if DEBUG_INSERTS_DPU_SIDE_STITCHING == 1

            if (conn_cmd->expected_UUID_version != parent_inner->UUID_version) {
                LOG_E("DPA: STITCH_UPDATE_LEARNED_SUBTREE parent=%p idx=%u UUID_WRONG. stitch_into_root=%u parent_UUID=%lu expected_UUID=%lu stitcher_thread=%lu\n",
                    parent_inner,
                    (unsigned int)child_index,
                    (unsigned int)conn_cmd->stitch_into_root,
                    (unsigned long)parent_inner->UUID_version,
                    (unsigned long)conn_cmd->expected_UUID_version,
                    stitcher_thread);                
            }

            // SANITY CHECK
            // check if new child first key stayed same.
            if (!conn_cmd->new_child_ptr) {
                LOG_E("DPA: STITCH_UPDATE_LEARNED_SUBTREE parent=%p idx=%u received null child pointer\n",
                      parent_inner,
                      (unsigned int)child_index);
                break;
            }

            DPALearnedTreeNode *new_child_node =
                (DPALearnedTreeNode *)conn_cmd->new_child_ptr;


            uint8_t node_type = new_child_node->base.type;


            uint64_t new_first_key = UINT64_MAX;

            if (node_type == DPA_LEARNED_LEAF) {
                new_first_key = ((DPALearnedLeafNode *)new_child_node)->first_key;
            } else if (node_type == DPA_LEARNED_INNER) {
                new_first_key = ((DPALearnedInnerNode *)new_child_node)->min_key;
            } else {
                LOG_E("DPA: STITCH_UPDATE_LEARNED_SUBTREE parent=%p idx=%u unknown child type=%u\n",
                      parent_inner,
                      (unsigned int)child_index,
                      (unsigned int)node_type);
            }

            if (stored_first_key != UINT64_MAX && new_first_key != UINT64_MAX &&
                stored_first_key != new_first_key) {
                LOG_E("DPA: STITCH_UPDATE_LEARNED_SUBTREE parent=%p idx=%u child=%p first key MISMATCH stored=%lu new=%lu expected=%lu root_stitch_race_result=%u stitch_into_root=%u stitcher_thread=%lu\n",
                      parent_inner,
                      (unsigned int)child_index,
                      new_child_node,
                      (unsigned long)stored_first_key,
                      (unsigned long)new_first_key,
                      conn_cmd->expected_first_key,
                      (unsigned int)root_stitch_race_result,
                      (unsigned int)conn_cmd->stitch_into_root,
                      stitcher_thread);
            } 
            // else {
            //     LOG_I("DPA: STITCH_UPDATE_LEARNED_SUBTREE parent=%p idx=%u child first key match stored=%lu new=%lu\n",
            //           parent_inner,
            //           (unsigned int)child_index,
            //           (unsigned long)stored_first_key,
            //           (unsigned long)new_first_key);
            // }

#endif // DEBUG_INSERTS_DPU_SIDE_STITCHING

            break;
        }

        case CMD_TYPE_STITCH_UNFLAG_LEAF: {
            command_stitch_unflag_leaf_t *unflag_cmd = (command_stitch_unflag_leaf_t *)cmd;
            DPALearnedLeafNode *leaf = (DPALearnedLeafNode *)unflag_cmd->dpa_leaf_ptr;

#if DEBUG_INSERTS_DPU_SIDE_STITCHING == 1

            if (!leaf) {
                LOG_I("DPA: STITCH_UNFLAG_LEAF received with null leaf pointer (host=%p)\n",
                      unflag_cmd->host_leaf_ptr);
                break;
            }

#endif // DEBUG_INSERTS_DPU_SIDE_STITCHING

            dpa_spinlock_acquire(&leaf->buffer_lock);
            leaf->buffer_high = 0;
            leaf->update_in_progress = 0;
            // for (uint32_t i = 0; i < LEARNED_INSERT_BUFFER_SIZE; i++) {
            //     leaf->buffer_keys[i] = 0;
            //     leaf->buffer_values[i] = 0;
            // }
            dpa_spinlock_release(&leaf->buffer_lock);

            break;
        }

        default: {
            // LOG_I("DPA: STITCH ERROR - Unknown command type: %u, size: %u\n",
            //      cmd->type, cmd->size);
            
            // // Print ringbuffer state
            // LOG_I("DPA: Ringbuffer state - head: %lu, tail: %lu\n",
            //       hga->stitch_queue_metadata.head, hga->stitch_queue_metadata.tail);
            // // Print first few bytes of the command for debugging
            // uint8_t *bytes = (uint8_t*)cmd;
            // LOG_I("DPA: First 32 bytes: ");
            // for (int i = 0; i < 32 && i < cmd->size; i++) {
            //     if (i > 0 && i % 8 == 0) LOG_I(" ");
            //     LOG_I("%02x", bytes[i]);
            // }
            // LOG_I("\n");
            return;
        }
    }
}

#endif // __DPASTORE_STITCHER_H__
