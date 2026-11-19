#include <doca_log.h>
DOCA_LOG_REGISTER(HOST_LEARNED_TREE)

#include "host_learned_tree.hpp"
#include "dpatree_allocator.hpp"
#include "piecewise_linear_model.hpp"
#include "../shared/commands.h"
#include "../shared/host_global_area.h"
#include "../shared/stitch_queue_guard.hpp"
#include "../shared/server_config.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <queue>
#include <thread>
#include <chrono>
#include <utility>
#include <unordered_set>
#include <vector>
#include <cstddef>
#include <limits>
#include <sstream>
#include <string>
#include <array>


// PLABuilder implementation - Now using PGM's optimal algorithm

namespace {

uint64_t steady_now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

template <typename KeyAccessor>
std::vector<PLASegment> build_pgm_segments_common(size_t count, int epsilon,
                                                  size_t max_segment_size,
                                                  KeyAccessor&& key_accessor) {
    using namespace pgm::internal;
    std::vector<PLASegment> segments;

    if (count == 0) return segments;

    auto in_fun = [&key_accessor](size_t i) { return key_accessor(i); };
    auto out_fun = [&segments](const auto& canonical_segment) {
        auto x_start = canonical_segment.get_first_x();
        auto [slope, intercept] = canonical_segment.get_floating_point_segment(x_start);
        // The intercept returned here is the predicted global position at first_key, keeping it integral.

        PLASegment seg;
        seg.first_key = x_start;
        seg.slope = slope;
        seg.intercept = static_cast<double>(intercept);
        segments.push_back(seg);
    };

    if (max_segment_size < SIZE_MAX) {
        make_segmentation_with_max_size(count, epsilon, max_segment_size, in_fun, out_fun);
    } else {
        make_segmentation(count, epsilon, in_fun, out_fun);
    }

    size_t position = 0;
    for (size_t i = 0; i < segments.size();) {
        segments[i].first_position = position;

        while (position < count &&
               (i + 1 == segments.size() || key_accessor(position) < segments[i + 1].first_key)) {
            position++;
        }

        size_t segment_start = segments[i].first_position;
        size_t segment_end = position;  // exclusive

        if (segment_start >= count) {
            segments.erase(segments.begin() + i);
            continue;
        }

        if (segment_end <= segment_start) {
            segment_end = std::min(count, segment_start + 1);
            position = std::max(position, segment_end);
        }

        segments[i].last_position = segment_end - 1;
        segments[i].first_key = key_accessor(segment_start);
        ++i;
    }

    return segments;
}

std::pair<double, double> compute_local_model(const PLASegment& segment,
                                              uint64_t segment_first_key,
                                              size_t base_index) {
    double slope = segment.slope;
    (void)segment_first_key;  // intercept already accounts for the first key
    double intercept = segment.intercept - static_cast<double>(base_index);
    return {slope, intercept};
}
/**
 * Compute the maximum number of elements allowed in each segment when rebuilding a node.
 *
 * The physical capacity (e.g., keys per leaf) is the hard limit we must never exceed.
 * When a load factor < 1.0 is supplied we try to leave slack so future inserts are less
 * likely to trigger another split. We calculate how many segments would be required to
 * satisfy both the physical capacity and the load-factor target, clamp the result to at
 * least one element, and return the effective per-segment cap. When the input is already
 * below capacity and `enforce_when_underfull` is false, the physical cap is returned so
 * perfectly fitting nodes are left untouched; if PGM splits this node anyway, the balancing is
 * achieved through its split results.
 */
size_t compute_balanced_segment_cap(size_t total_items,
                                    size_t physical_cap,
                                    double load_factor,
                                    bool enforce_when_underfull) {
    if (physical_cap == 0) {
        return 0;
    }
    if (total_items == 0) {
        return 1;
    }
    if (load_factor <= 0.0 || load_factor >= 1.0) {
        return physical_cap;
    }
    if (!enforce_when_underfull && total_items <= physical_cap) {
        return physical_cap;
    }

    double effective_cap = static_cast<double>(physical_cap) * load_factor;
    if (effective_cap < 1.0) {
        effective_cap = 1.0;
    }

    size_t min_segments = (total_items + physical_cap - 1) / physical_cap;
    size_t load_constrained_segments = static_cast<size_t>(
        std::ceil(static_cast<double>(total_items) / effective_cap));
    size_t target_segments = std::max<size_t>(
        min_segments, std::max<size_t>(1, load_constrained_segments));

    size_t cap = static_cast<size_t>(
        std::ceil(static_cast<double>(total_items) / target_segments));
    cap = std::max<size_t>(1, cap);
    return std::min<size_t>(physical_cap, cap);
}

/**
 * Partition a list of PGM leaf segments into per-inner-node groups that respect fanout limits.
 *
 * `segments` describes the consecutive child ranges produced by PGM. We walk them from left
 * to right, emitting group sizes such that:
 *   - No group exceeds `max_segments_per_node` (hardware-defined limit, e.g., 7).
 *   - We reserve enough segments for remaining nodes so the tail groups stay within bounds.
 *   - Among the feasible group sizes we pick the one whose covered child span is closest to a
 *     uniform share of the remaining children, which helps keep each inner evenly loaded.
 *
 * The returned vector contains the number of segments assigned to each new inner node in order.
 */
std::vector<size_t> compute_inner_segment_groups(const std::vector<PLASegment>& segments,
                                                 size_t total_children,
                                                 size_t max_segments_per_node) {
    std::vector<size_t> groups;
    if (segments.empty()) {
        return groups;
    }
    if (max_segments_per_node == 0) {
        return groups;
    }

    size_t total_segments = segments.size();
    size_t nodes_needed = (total_segments + max_segments_per_node - 1) / max_segments_per_node;
    nodes_needed = std::max<size_t>(1, nodes_needed);

    size_t seg_index = 0;
    size_t nodes_remaining = nodes_needed;
    while (seg_index < total_segments && nodes_remaining > 0) {
        size_t remaining_segments = total_segments - seg_index;
        size_t remaining_nodes = nodes_remaining;

        size_t max_segments_this_node = std::min(max_segments_per_node, remaining_segments);

        size_t min_segments_this_node = 1;
        if (remaining_nodes == 1) {
            min_segments_this_node = remaining_segments;
        } else {
            size_t max_segments_for_rest = max_segments_per_node * (remaining_nodes - 1);
            if (remaining_segments > max_segments_for_rest) {
                size_t needed = remaining_segments - max_segments_for_rest;
                min_segments_this_node = std::max<size_t>(1, needed);
            }
        }
        if (min_segments_this_node > max_segments_this_node) {
            min_segments_this_node = max_segments_this_node;
        }

        size_t node_first_child = segments[seg_index].first_position;
        size_t remaining_children = total_children > node_first_child
                                        ? total_children - node_first_child
                                        : 0;
        size_t target_children = remaining_nodes > 0
                                     ? std::max<size_t>(
                                           1, (remaining_children + remaining_nodes - 1) / remaining_nodes)
                                     : remaining_children;

        size_t best_count = min_segments_this_node;
        size_t best_diff = std::numeric_limits<size_t>::max();

        for (size_t candidate = min_segments_this_node; candidate <= max_segments_this_node; ++candidate) {
            if (seg_index + candidate > segments.size()) {
                break;
            }

            size_t segments_after = remaining_segments - candidate;
            size_t nodes_after = remaining_nodes > 1 ? remaining_nodes - 1 : 0;
            if (nodes_after > 0 &&
                segments_after > max_segments_per_node * nodes_after) {
                // This candidate leaves too many segments for the remaining nodes.
                continue;
            }

            size_t last_idx = seg_index + candidate - 1;
            size_t node_last_child_plus1 =
                std::min(total_children, segments[last_idx].last_position + 1);
            size_t assigned_children =
                node_last_child_plus1 > node_first_child
                    ? node_last_child_plus1 - node_first_child
                    : 0;

            size_t diff = assigned_children > target_children
                              ? assigned_children - target_children
                              : target_children - assigned_children;

            if (diff < best_diff) {
                best_diff = diff;
                best_count = candidate;
            } else if (diff == best_diff && candidate < best_count) {
                best_count = candidate;
            }
        }

        if (best_diff == std::numeric_limits<size_t>::max()) {
            best_count = min_segments_this_node;
        }

        groups.push_back(best_count);
        seg_index += best_count;
        if (nodes_remaining > 0) {
            nodes_remaining--;
        }
    }

    return groups;
}

}  // namespace

std::vector<PLASegment> PLABuilder::build_segments_for_kvpairs(const KVPair* data, size_t count,
                                                               int epsilon, size_t max_size) {
    auto key_accessor = [data](size_t i) { return data[i].key; };
    auto segments = build_pgm_segments_common(count, epsilon / 2, max_size, key_accessor);

    // DOCA_LOG_INFO("Leaf segmentation: %zu keys -> %zu segments (avg %.1f keys/segment, max %zu)",
    //               count, segments.size(),
    //               segments.empty() ? 0.0 : static_cast<double>(count) / segments.size(),
    //               max_size);
    // if (segments.size() >= 2 && segments.size() <= 50) {
    //     for (size_t i = 0; i < segments.size(); i++) {
    //         const auto& seg = segments[i];
    //         size_t segment_keys = seg.last_position >= seg.first_position
    //                                   ? (seg.last_position - seg.first_position + 1)
    //                                   : 0;
    //         DOCA_LOG_INFO("  segment[%zu]: first_key=%lu last_key=%lu first_pos=%zu last_pos=%zu keys=%zu slope=%.6f intercept=%.6f",
    //                       i,
    //                       static_cast<unsigned long>(seg.first_key),
    //                       static_cast<unsigned long>(seg.last_key),
    //                       seg.first_position,
    //                       seg.last_position,
    //                       segment_keys,
    //                       seg.slope,
    //                       seg.intercept);
    //     }
    // }

    return segments;
}

std::vector<PLASegment> PLABuilder::build_segments_for_children(HostLearnedTreeNode** children,
                                                               size_t count, int epsilon) {
    std::vector<uint64_t> first_keys;
    first_keys.reserve(count);

    for (size_t i = 0; i < count; i++) {
        if (!children[i]) continue;

        if (children[i]->type == HostLearnedTreeNode::LEARNED_LEAF) {
            auto* leaf = static_cast<HostLearnedLeafNode*>(children[i]);
            first_keys.push_back(leaf->first_key);
        } else {
            auto* inner = static_cast<HostLearnedInnerNode*>(children[i]);
            first_keys.push_back(inner->min_key);
        }
    }

    auto key_accessor = [&first_keys](size_t i) { return first_keys[i]; };
    return build_pgm_segments_common(first_keys.size(), epsilon, SIZE_MAX, key_accessor);
}

// HostLearnedTree implementation

HostLearnedTree::HostLearnedTree(void* leaf_buffer, size_t leaf_buffer_size,
                                 void* inner_buffer, size_t inner_buffer_size,
                                 void* host_global_area, 
                                 const std::vector<void*>& dpa_segments, size_t segment_size)
    : root_(nullptr)
    , root_epoch_(0)
    , num_leafs_(0)
    , num_inners_(0)
    , num_entries_(0)
    , patches_processed_(0)
    , structural_modifications_began(0)
    , structural_modifications_enqueued(0)
    , host_global_area_(host_global_area)
    , dpa_base_addr_(!dpa_segments.empty() ? dpa_segments[0] : nullptr) {
    
    // Initialize pool allocators
    leaf_pool_ = new PoolAllocator(leaf_buffer, sizeof(HostLearnedLeafNode), leaf_buffer_size);
    inner_pool_ = new PoolAllocator(inner_buffer, sizeof(HostLearnedInnerNode), inner_buffer_size);
    
    // Initialize DPA allocator for shadow allocation with multiple segments
    if (!dpa_segments.empty()) {
        dpa_allocator_ = new DPATreeAllocator(dpa_segments, segment_size);
    } else {
        dpa_allocator_ = nullptr;
    }
}

HostLearnedTree::~HostLearnedTree() {
    delete leaf_pool_;
    delete inner_pool_;
    delete dpa_allocator_;
}

HostLearnedLeafNode* HostLearnedTree::allocate_leaf() {
    void* mem = leaf_pool_->allocate();
    if (!mem) return nullptr;
    
    auto* leaf = new (mem) HostLearnedLeafNode();
    {
        //SpinLockGuard guard(stats_lock_);
        num_leafs_++;
    }
    return leaf;
}

HostLearnedInnerNode* HostLearnedTree::allocate_inner() {
    void* mem = inner_pool_->allocate();
    if (!mem) return nullptr;
    
    auto* inner = new (mem) HostLearnedInnerNode();
    {
        //SpinLockGuard guard(stats_lock_);
        num_inners_++;
    }
    return inner;
}

void HostLearnedTree::deallocate_node(HostLearnedTreeNode* node) {
    if (!node) return;

    if (dpa_allocator_) {
        if (node->type == HostLearnedTreeNode::LEARNED_LEAF) {
            dpa_allocator_->release_learned_leaf(node);
        } else {
            dpa_allocator_->release_learned_inner(node);
        }
    }

    if (node->type == HostLearnedTreeNode::LEARNED_LEAF) {
        leaf_pool_->deallocate(node);
        //SpinLockGuard guard(stats_lock_);
        num_leafs_--;
    } else {
        inner_pool_->deallocate(node);
        //SpinLockGuard guard(stats_lock_);
        num_inners_--;
        // DOCA_LOG_INFO("DEALLOC");
    }
}

void* HostLearnedTree::ensure_dpa_mapping(HostLearnedTreeNode* node) {
    if (!node || !dpa_allocator_) {
        return nullptr;
    }

    if (node->type == HostLearnedTreeNode::LEARNED_LEAF) {
        return dpa_allocator_->ensure_learned_leaf(node);
    }

    return dpa_allocator_->ensure_learned_inner(node);
}

uint32_t HostLearnedTree::queue_for_partition(uint16_t partition_id) const {
    if (NUM_STITCH_QUEUES == 0) {
        return 0;
    }
    return static_cast<uint32_t>(partition_id % NUM_STITCH_QUEUES);
}

uint32_t HostLearnedTree::queue_for_leaf(const HostLearnedLeafNode* leaf) const {
    if (NUM_STITCH_QUEUES == 0) {
        return 0;
    }
    if (!leaf) {
        return 0;
    }
    return queue_for_partition(leaf->partition_id);
}

void HostLearnedTree::prepare_copy_stitch_commands(uint32_t queue_id,
                                                   const std::vector<HostLearnedTreeNode*>& nodes,
                                                   bool print_debug,
                                                   std::vector<PreparedCopyStitchCommand>& out_commands) {
#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (print_debug) { DOCA_LOG_INFO("[stitch] prepare_copy_stitch_commands enter (queue=%u)", queue_id); }
#endif // DEBUG_INSERTS_MULTI_PATCHER 

    if (nodes.empty()) {
        return;
    }

    out_commands.reserve(out_commands.size() + nodes.size());

    for (auto* node : nodes) {
        if (!node) {
            continue;
        }
#if DEBUG_INSERTS_MULTI_PATCHER == 1
        if (print_debug) { DOCA_LOG_INFO("[stitch] prepare_copy_stitch_commands node=%p", node); }
#endif // DEBUG_INSERTS_MULTI_PATCHER  

        void* dpa_ptr = ensure_dpa_mapping(node);
#if DEBUG_INSERTS_MULTI_PATCHER == 1
        if (!dpa_ptr) {
            DOCA_LOG_ERR("Failed to allocate/translate DPA pointer for node %p", node);
            continue;
        }

        if (print_debug) { DOCA_LOG_INFO("[stitch] prepare_copy_stitch_commands after mapping)"); }
#endif // DEBUG_INSERTS_MULTI_PATCHER  

        PreparedCopyStitchCommand prepared{};

        if (node->type == HostLearnedTreeNode::LEARNED_LEAF) {
            auto* leaf = static_cast<HostLearnedLeafNode*>(node);

            auto* cmd = reinterpret_cast<command_stitch_copy_learned_leaf_t*>(prepared.buffer.data());
            *cmd = command_stitch_copy_learned_leaf_t{};
            cmd->size = sizeof(*cmd);
            cmd->type = CMD_TYPE_STITCH_COPY_LEARNED_LEAF;
            cmd->dpa_ptr = dpa_ptr;
            cmd->host_ptr = leaf;
            cmd->sorted_count = leaf->sorted_count;
            cmd->slope_fp = leaf->slope_fp;
            cmd->intercept_fp = leaf->intercept_fp;
            cmd->first_key = leaf->first_key;
            cmd->UUID_version = leaf->UUID_version;

#ifndef USE_MICRO_LEAVES
            for (uint32_t i = 0; i < leaf->sorted_count && i < LEARNED_KEYS_PER_LEAF; i++) {
                cmd->sorted_entries[i].key = leaf->sorted_keys[i];
                cmd->sorted_entries[i].value = leaf->sorted_values[i];
            }

            for (uint32_t i = leaf->sorted_count; i < LEARNED_KEYS_PER_LEAF; i++) {
                cmd->sorted_entries[i].key = 0;
                cmd->sorted_entries[i].value = 0;
            }
#endif // USE_MICRO_LEAVES

            prepared.size = cmd->size;
            prepared.type = static_cast<command_type_t>(cmd->type);
            out_commands.push_back(prepared);
            continue;
        }

        auto* inner = static_cast<HostLearnedInnerNode*>(node);

#if DEBUG_INSERTS_MULTI_PATCHER == 1
        DOCA_LOG_INFO("[stitch] Enqueue learned inner copy host=%p -> dpa=%p (segments=%u, children=%u, min_key=%lu, partition=%u, queue_id=%u)",
                      inner,
                      dpa_ptr,
                      inner->num_segments,
                      inner->num_children,
                      inner->min_key,
                      inner->partition_id,
                      queue_id);
#endif // DEBUG_INSERTS_MULTI_PATCHER  

        auto* cmd = reinterpret_cast<command_stitch_copy_learned_inner_t*>(prepared.buffer.data());
        *cmd = command_stitch_copy_learned_inner_t{};
        cmd->size = sizeof(*cmd);
        cmd->type = CMD_TYPE_STITCH_COPY_LEARNED_INNER;
        cmd->dpa_ptr = dpa_ptr;
        cmd->num_segments = inner->num_segments;
        cmd->num_children = inner->num_children;
        cmd->tree_level = 0; // depth encoded during dispatch on device
        cmd->min_key = inner->min_key;
        cmd->UUID_version = inner->UUID_version;

        for (uint16_t i = 0; i < inner->num_segments && i < LEARNED_SEGMENTS_PER_INNER; i++) {
            cmd->segments[i].first_key = inner->segments[i].first_key;
            cmd->segments[i].slope_fp = inner->segments[i].slope_fp;
            cmd->segments[i].intercept_fp = inner->segments[i].intercept_fp;
            cmd->segments[i].first_child_idx = inner->segments[i].first_child_idx;
            cmd->segments[i].num_children = inner->segments[i].num_children;
        }
        for (uint16_t i = inner->num_segments; i < LEARNED_SEGMENTS_PER_INNER; i++) {
            cmd->segments[i] = stitch_learned_segment_t{};
        }

        const size_t max_children = LEARNED_SEGMENTS_PER_INNER * LEARNED_KEYS_PER_INNER_SEGMENT;
#if DEBUG_INSERTS_MULTI_PATCHER == 1
        if (print_debug) { DOCA_LOG_INFO("[stitch] prepare_copy_stitch_commands before child mapping)"); }
#endif // DEBUG_INSERTS_MULTI_PATCHER  
        for (uint16_t i = 0; i < inner->num_children && i < max_children; i++) {
            HostLearnedTreeNode* child = inner->children[i];
            if (child) {
                if (child->type == HostLearnedTreeNode::LEARNED_INNER) {
                    cmd->child_first_keys[i] = static_cast<HostLearnedInnerNode*>(child)->min_key;
                } else {
                    cmd->child_first_keys[i] = static_cast<HostLearnedLeafNode*>(child)->first_key;
                }
                cmd->children[i] = ensure_dpa_mapping(child);
            } else {
                cmd->child_first_keys[i] = UINT64_MAX;
                cmd->children[i] = nullptr;
            }
        }
#if DEBUG_INSERTS_MULTI_PATCHER == 1
        if (print_debug) { DOCA_LOG_INFO("[stitch] prepare_copy_stitch_commands after child mapping)"); }
#endif // DEBUG_INSERTS_MULTI_PATCHER  

        for (uint16_t i = inner->num_children; i < max_children; i++) {
            cmd->child_first_keys[i] = UINT64_MAX;
            cmd->children[i] = nullptr;
        }

        prepared.size = cmd->size;
        prepared.type = static_cast<command_type_t>(cmd->type);
        out_commands.push_back(prepared);
    }
}

void HostLearnedTree::emit_copy_stitches_on_queue(uint32_t queue_id,
                                                  const std::vector<PreparedCopyStitchCommand>& commands,
                                                  bool print_debug) {
#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (print_debug) { DOCA_LOG_INFO("[stitch] emit_copy_stitches_on_queue enter (queue=%u)", queue_id); }
#endif // DEBUG_INSERTS_MULTI_PATCHER  
    if (commands.empty()) {
        return;
    }

    auto* hga = static_cast<host_global_area_t*>(host_global_area_);
#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (!hga) {
        DOCA_LOG_ERR("emit_copy_stitches_on_queue called without host global area");
        return;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER 

    for (const auto& prepared : commands) {
#if DEBUG_INSERTS_MULTI_PATCHER == 1
        if (print_debug) {
            DOCA_LOG_INFO("[stitch] emit_copy_stitches_on_queue sending type=%u size=%u",
                          static_cast<unsigned>(prepared.type),
                          prepared.size);
        }
#endif // DEBUG_INSERTS_MULTI_PATCHER  
        host_send_stitch_thread_safe(hga, queue_id, prepared.buffer.data(), prepared.size, print_debug);
#if DEBUG_INSERTS_MULTI_PATCHER == 1
        if (print_debug) { DOCA_LOG_INFO("[stitch] emit_copy_stitches_on_queue sent)"); }
#endif // DEBUG_INSERTS_MULTI_PATCHER 
    }
}

void HostLearnedTree::emit_copy_stitches(const std::vector<HostLearnedTreeNode*>& nodes) {
    if (nodes.empty()) {
        return;
    }

    if (NUM_STITCH_QUEUES <= 1) {
        std::vector<PreparedCopyStitchCommand> commands;
        prepare_copy_stitch_commands(0, nodes, false, commands);
        emit_copy_stitches_on_queue(0, commands);
        return;
    }

    std::array<std::vector<HostLearnedTreeNode*>, NUM_STITCH_QUEUES> buckets;
    for (auto* node : nodes) {
        if (!node) {
            continue;
        }

        uint32_t queue_id = 0;
        if (node->type == HostLearnedTreeNode::LEARNED_INNER) {
            queue_id = queue_for_partition(static_cast<HostLearnedInnerNode*>(node)->partition_id);
        } else {
            queue_id = queue_for_leaf(static_cast<HostLearnedLeafNode*>(node));
        }
        buckets[queue_id].push_back(node);
    }

    for (uint32_t queue_id = 0; queue_id < NUM_STITCH_QUEUES; ++queue_id) {
        if (!buckets[queue_id].empty()) {
            std::vector<PreparedCopyStitchCommand> commands;
            prepare_copy_stitch_commands(queue_id, buckets[queue_id], false, commands);
            emit_copy_stitches_on_queue(queue_id, commands);
        }
    }
}

void HostLearnedTree::send_connect_stitch_on_queue(HostLearnedInnerNode* parent,
                                                   HostLearnedTreeNode* child,
                                                   uint16_t child_index,
                                                   uint32_t queue_id) {
    auto* hga = static_cast<host_global_area_t*>(host_global_area_);
#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (!hga) {
        DOCA_LOG_ERR("send_connect_stitch_on_queue without host global area");
        return;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER 

    void* parent_dpa = ensure_dpa_mapping(parent);
    void* child_dpa = ensure_dpa_mapping(child);
#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (!parent_dpa || !child_dpa) {
        DOCA_LOG_ERR("send_connect_stitch_on_queue failed to ensure DPA mapping (parent=%p child=%p)", parent, child);
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER 


#if DEBUG_INSERTS_MULTI_PATCHER == 1
    DOCA_LOG_INFO("[stitch] Enqueue subtree update parent host=%p (dpa=%p) idx=%u -> child host=%p (dpa=%p) queue=%u",
                  parent,
                  parent_dpa,
                  child_index,
                  child,
                  child_dpa,
                  queue_id);
#endif // DEBUG_INSERTS_MULTI_PATCHER 

    HostLearnedTreeNode* root_snapshot = root_.load(std::memory_order_acquire);

    uint16_t stitch_root_flag = 0;
    if (parent == root_snapshot) {
#if DEBUG_INSERTS_MULTI_PATCHER == 1
        DOCA_LOG_INFO("[stitch]    ... parent is root.");
#endif // DEBUG_INSERTS_MULTI_PATCHER 
        stitch_root_flag = 1;
    }

    if (parent->result_from_root_split == 1) {
        stitch_root_flag = 1;
    }

    command_stitch_update_learned_subtree_t cmd{};
    cmd.size = sizeof(cmd);
    cmd.type = CMD_TYPE_STITCH_UPDATE_LEARNED_SUBTREE;
    cmd.parent_ptr = parent_dpa;
    cmd.new_child_ptr = child_dpa;
    cmd.child_index = child_index;
    cmd.stitch_into_root = stitch_root_flag;
    cmd.expected_UUID_version = parent->UUID_version;

    if (child->type == HostLearnedTreeNode::LEARNED_LEAF) {
        cmd.expected_first_key = ((HostLearnedLeafNode *)child)->first_key;
    } else if (child->type == HostLearnedTreeNode::LEARNED_INNER) {
        cmd.expected_first_key = ((HostLearnedInnerNode *)child)->min_key;
    }

        host_send_stitch_thread_safe(hga, queue_id, &cmd, sizeof(cmd));
}

void HostLearnedTree::send_connect_stitch(HostLearnedInnerNode* parent,
                                          HostLearnedTreeNode* child,
                                          uint16_t child_index) {
    uint32_t queue_id = queue_for_partition(parent ? parent->partition_id : 0);
    send_connect_stitch_on_queue(parent, child, child_index, queue_id);
}

void HostLearnedTree::broadcast_epoch_stitch(uint64_t epoch) {
    if (NUM_STITCH_QUEUES == 0) {
        return;
    }

    auto* hga = static_cast<host_global_area_t*>(host_global_area_);
#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (!hga) {
        DOCA_LOG_ERR("broadcast_epoch_stitch without host global area");
        return;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER 
    command_stitch_set_epoch_t cmd{};
    cmd.size = sizeof(cmd);
    cmd.type = CMD_TYPE_STITCH_SET_EPOCH;
    cmd.epoch = epoch;

#if DEBUG_INSERTS_MULTI_PATCHER == 1
    DOCA_LOG_INFO("[stitch] Enqueue broadcast stitch, epoch=%ul)",
                  epoch);
#endif // DEBUG_INSERTS_MULTI_PATCHER 

    for (uint32_t queue_id = 0; queue_id < NUM_STITCH_QUEUES; ++queue_id) {
        host_send_stitch_thread_safe(hga, queue_id, &cmd, sizeof(cmd));
    }
}

void HostLearnedTree::send_root_stitch_on_queue(HostLearnedTreeNode* new_root,
                                                uint32_t queue_id) {
    auto* hga = static_cast<host_global_area_t*>(host_global_area_);
#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (!hga) {
        DOCA_LOG_ERR("send_root_stitch_on_queue without host global area");
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER 

    void* root_dpa = ensure_dpa_mapping(new_root);
#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (!root_dpa) {
        DOCA_LOG_ERR("send_root_stitch_on_queue failed to translate root %p", new_root);
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER 

#if DEBUG_INSERTS_MULTI_PATCHER == 1
    DOCA_LOG_INFO("[stitch] Enqueue root update host=%p (dpa=%p) queue=%u",
                  new_root,
                  root_dpa,
                  queue_id);
#endif // DEBUG_INSERTS_MULTI_PATCHER 

    uint64_t barrier_epoch = root_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    broadcast_epoch_stitch(barrier_epoch);

    command_stitch_update_learned_root_t cmd{};
    cmd.size = sizeof(cmd);
    cmd.type = CMD_TYPE_STITCH_UPDATE_LEARNED_ROOT;
    cmd.new_root = root_dpa;
    cmd.expected_UUID_version = new_root->UUID_version;
    cmd.barrier_epoch = barrier_epoch;

    host_send_stitch_thread_safe(hga, queue_id, &cmd, sizeof(cmd));
}

void HostLearnedTree::send_root_stitch(HostLearnedTreeNode* new_root) {
    uint32_t queue_id = 0;
    if (new_root) {
        if (new_root->type == HostLearnedTreeNode::LEARNED_INNER) {
            queue_id = queue_for_partition(static_cast<HostLearnedInnerNode*>(new_root)->partition_id);
        } else {
            queue_id = queue_for_leaf(static_cast<HostLearnedLeafNode*>(new_root));
        }
    }
    send_root_stitch_on_queue(new_root, queue_id);
}

void HostLearnedTree::send_unflag_stitch_on_queue(HostLearnedLeafNode* leaf, uint32_t queue_id) {
#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (!leaf) {
        DOCA_LOG_ERR("send_unflag_stitch_on_queue called with null leaf");
        return;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER 

    auto* hga = static_cast<host_global_area_t*>(host_global_area_);
#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (!hga) {
        DOCA_LOG_ERR("send_unflag_stitch_on_queue without host global area");
        return;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER 

    void* leaf_dpa = ensure_dpa_mapping(leaf);
#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (!leaf_dpa) {
        DOCA_LOG_ERR("send_unflag_stitch_on_queue failed to translate leaf %p", leaf);
        return;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER 

    command_stitch_unflag_leaf_t cmd{};
    cmd.size = sizeof(cmd);
    cmd.type = CMD_TYPE_STITCH_UNFLAG_LEAF;
    cmd.dpa_leaf_ptr = leaf_dpa;
    cmd.host_leaf_ptr = leaf;

    host_send_stitch_thread_safe(hga, queue_id, &cmd, sizeof(cmd));
}

void HostLearnedTree::send_unflag_stitch(HostLearnedLeafNode* leaf) {
    uint32_t queue_id = queue_for_leaf(leaf);
    send_unflag_stitch_on_queue(leaf, queue_id);
}

HostLearnedLeafNode* HostLearnedTree::create_leaf_from_pgm_segment(const KVPair* data, size_t start, size_t end,
                                                                   const PLASegment& segment, int epsilon) {
    HostLearnedLeafNode* leaf = allocate_leaf();
    if (!leaf) return nullptr;

    size_t count = end - start;
    leaf->set_sorted_data(data + start, count);

    // The PGM segment model is emitted relative to the first key: global_position = slope * (key - first_key) + intercept
    // We need a model that maps: local_position = slope' * (key - first_key) + intercept'
    // Where local_position is in range [0, count-1]
    if (count > 1) {
        uint64_t first_key = data[start].key;

        // For a key k in this leaf:
        // PGM says: global_pos = segment.slope * (k - first_key) + segment.intercept
        // Local pos = global_pos - start
        // We want: local_pos = new_slope * (k - first_key) + new_intercept

        // Expanding with the new global model: local_pos = segment.slope * (k - first_key) + segment.intercept - start
        //
        // So: new_slope = segment.slope
        //     new_intercept = segment.intercept - start

        auto [new_slope, new_intercept] = compute_local_model(segment, first_key, start);

        // We keep the following commented, in case we will need it.
        // // The model should map to [0, count-1] range
        // // If predictions are outside this range, we need to adjust
        // if (last_predicted > count - 1 || first_predicted < 0) {
        //     // Recalculate to fit exactly in [0, count-1]
        //     new_slope = (count - 1) / static_cast<double>(last_key - first_key);
        //     new_intercept = 0;
        //     DOCA_LOG_INFO("  Adjusted model: slope=%.6f, intercept=%.6f", new_slope, new_intercept);
        // }

        leaf->set_model(new_slope, new_intercept);
        // Build epsilon is guaranteed by PGM segmentation

        // Debug: Check epsilon in LOCAL coordinates after transformation
        double max_local_error = 0;
        const double epsilon_limit = static_cast<double>(epsilon);
        for (size_t i = 0; i < count; i++) {
            uint64_t key = data[start + i].key;
            // Apply the local model
            double predicted = new_slope * (key - first_key) + new_intercept;
            double actual = static_cast<double>(i);  // Local position [0, count-1]
            double error = std::abs(predicted - actual);
            max_local_error = std::max(max_local_error, error);
        }
        if (max_local_error > epsilon_limit) {
            DOCA_LOG_WARN("  Leaf exceeds epsilon in LOCAL coords! Max error: %.2f > %.2f",
                          max_local_error, epsilon_limit);
        }
    } else {
        leaf->set_model(0, 0);
    }

    {
        //SpinLockGuard guard(stats_lock_);
        num_entries_ += count;
    }
    return leaf;
}

// Helper to create inner nodes from segments and children
std::vector<HostLearnedInnerNode*> HostLearnedTree::create_inner_nodes_from_segments(
    const std::vector<PLASegment>& pgm_segments,
    const std::vector<HostLearnedTreeNode*>& nodes,
    const std::vector<uint64_t>& first_keys,
    uint16_t error_bound) {

    std::vector<HostLearnedInnerNode*> inner_nodes;
    auto group_sizes = compute_inner_segment_groups(pgm_segments, nodes.size(),
                                                    LEARNED_SEGMENTS_PER_INNER);
    if (group_sizes.empty() && !pgm_segments.empty()) {
        DOCA_LOG_ERR("Failed to compute segment grouping for inner creation");
        return {};
    }

    size_t seg_index = 0;
    for (size_t group_idx = 0; group_idx < group_sizes.size(); ++group_idx) {
        size_t segments_this_node = group_sizes[group_idx];
        size_t seg_end = std::min(seg_index + segments_this_node, pgm_segments.size());

        if (segments_this_node > LEARNED_SEGMENTS_PER_INNER) {
            DOCA_LOG_ERR("Inner segment group exceeds per-node segment capacity");
            for (auto* allocated : inner_nodes) {
                deallocate_node(allocated);
            }
            return {};
        }

        std::vector<LearnedSegment> inner_segments;
        std::vector<HostLearnedTreeNode*> inner_children;

        for (size_t s = seg_index; s < seg_end; ++s) {
            const auto& pgm_seg = pgm_segments[s];

            size_t child_start = pgm_seg.first_position;
            size_t child_end = std::min(pgm_seg.last_position + 1, nodes.size());

            uint16_t first_child_idx = inner_children.size();
            uint16_t num_children = child_end > child_start
                                        ? static_cast<uint16_t>(child_end - child_start)
                                        : 0;

            for (size_t c = child_start; c < child_end; ++c) {
                inner_children.push_back(nodes[c]);
            }

            uint64_t segment_first_key = first_keys[child_start];
            auto [new_slope, new_intercept] =
                compute_local_model(pgm_seg, segment_first_key, child_start);

            double max_local_error = 0;
            for (size_t c = 0; c < num_children; c++) {
                uint64_t child_key = first_keys[child_start + c];
                double predicted = new_slope * (child_key - segment_first_key) + new_intercept;
                double actual = static_cast<double>(c);
                double error = std::abs(predicted - actual);
                max_local_error = std::max(max_local_error, error);
            }
            if (max_local_error > error_bound) {
                DOCA_LOG_WARN("  Inner segment %zu exceeds epsilon in LOCAL coords! Max error: %.2f > %.2f",
                              s, max_local_error, static_cast<double>(error_bound));
            }

            inner_segments.emplace_back(segment_first_key, new_slope, new_intercept,
                                        first_child_idx, num_children);
        }

        HostLearnedInnerNode* inner = allocate_inner();
        if (inner) {
            inner->~HostLearnedInnerNode();
            new (inner) HostLearnedInnerNode(inner_segments, inner_children, error_bound);
            inner_nodes.push_back(inner);
        }

        seg_index = seg_end;
    }

    return inner_nodes;
}

// Materialise the minimal set of HostLearnedInnerNode objects needed to cover
// the supplied children consecutively. Everything is built off to the side so
// callers can splice the result into the live tree atomically during updates.
std::vector<HostLearnedInnerNode*> HostLearnedTree::make_inner_nodes_for_children(
    const std::vector<HostLearnedTreeNode*>& children,
    uint16_t default_partition,
    bool inherit_partition_from_children) {
    std::vector<HostLearnedInnerNode*> new_nodes;

    if (children.empty()) {
        return new_nodes;
    }

    uint16_t assigned_partition = default_partition;
    if (inherit_partition_from_children) {
        bool partition_initialized = false;
        for (auto* child : children) {
#if DEBUG_INSERTS_MULTI_PATCHER == 1
            if (!child) {
                DOCA_LOG_ERR("Null child encountered while deriving partition for inner subtree");
                return {};
            }
#endif //DEBUG_INSERTS_MULTI_PATCHER

            uint16_t child_partition = child->partition_id;
            if (!partition_initialized) {
                assigned_partition = child_partition;
                partition_initialized = true;
            } 
#if DEBUG_INSERTS_MULTI_PATCHER == 1            
            else if (child_partition != assigned_partition) {
                DOCA_LOG_ERR(
                    "Partition mismatch while building inner subtree (child partition %u != %u)",
                    child_partition,
                    assigned_partition);
                return {};
            }
#endif //DEBUG_INSERTS_MULTI_PATCHER
        }
    }

const size_t max_children = LEARNED_SEGMENTS_PER_INNER * LEARNED_KEYS_PER_INNER_SEGMENT;

#if DEBUG_INSERTS_MULTI_PATCHER == 1      
    // Hard guard against unbounded fanout. It should never trigger in practice, but
    // catches pathological input before we start allocating inner nodes.
    if (children.size() > max_children * 4) {
        DOCA_LOG_ERR("Requested inner subtree exceeds supported fanout (%zu)", children.size());
        return {};
    }
#endif //DEBUG_INSERTS_MULTI_PATCHER

#if DEBUG_INSERTS_MULTI_PATCHER == 1   
    auto dump_children_state = [&children]() {
        for (size_t dump_idx = 0; dump_idx < children.size(); ++dump_idx) {
            HostLearnedTreeNode* dump_child = children[dump_idx];
            if (!dump_child) {
                DOCA_LOG_ERR("  child[%zu]: <null>", dump_idx);
                continue;
            }

            uint64_t dump_key = 0;
            size_t dump_entries = 0;
            const char* dump_type =
                dump_child->type == HostLearnedTreeNode::LEARNED_LEAF ? "leaf" : "inner";

            if (dump_child->type == HostLearnedTreeNode::LEARNED_LEAF) {
                auto* dump_leaf = static_cast<HostLearnedLeafNode*>(dump_child);
                dump_key = dump_leaf->first_key;
                dump_entries = dump_leaf->sorted_count;
            } else {
                auto* dump_inner = static_cast<HostLearnedInnerNode*>(dump_child);
                dump_key = dump_inner->min_key;
                dump_entries = dump_inner->num_children;
            }

            DOCA_LOG_ERR("  child[%zu] (%s): first_key=%lu entries=%zu",
                         dump_idx,
                         dump_type,
                         static_cast<unsigned long>(dump_key),
                         dump_entries);
        }
    };
#endif //DEBUG_INSERTS_MULTI_PATCHER

    std::vector<uint64_t> first_keys;
    first_keys.reserve(children.size());

    for (size_t idx = 0; idx < children.size(); ++idx) {
        HostLearnedTreeNode* child = children[idx];
#if DEBUG_INSERTS_MULTI_PATCHER == 1   
        if (!child) {
            DOCA_LOG_ERR("Null child encountered while building inner subtree");
            return {};
        }
#endif //DEBUG_INSERTS_MULTI_PATCHER

        uint64_t key = 0;
        if (child->type == HostLearnedTreeNode::LEARNED_LEAF) {
            auto* leaf = static_cast<HostLearnedLeafNode*>(child);
#if DEBUG_INSERTS_MULTI_PATCHER == 1   
            if (leaf->sorted_count == 0) {
                DOCA_LOG_ERR("Leaf child has no sorted entries while building inner subtree");
                return {};
            }
#endif //DEBUG_INSERTS_MULTI_PATCHER
            key = leaf->first_key;
        } else {
            auto* inner = static_cast<HostLearnedInnerNode*>(child);
            key = inner->min_key;
        }

#if DEBUG_INSERTS_MULTI_PATCHER == 1   
        if (!first_keys.empty() && key < first_keys.back()) {
            DOCA_LOG_ERR("Child keys out of order while building inner subtree (index %zu, key %lu < prev %lu)",
                         idx,
                         static_cast<unsigned long>(key),
                         static_cast<unsigned long>(first_keys.back()));
            dump_children_state();
            return {};
        }

        if (!first_keys.empty() && key == first_keys.back()) {
            DOCA_LOG_ERR("Duplicate child key while building inner subtree (index %zu, key %lu)",
                         idx,
                         static_cast<unsigned long>(key));
            dump_children_state();
            return {};
        }
#endif //DEBUG_INSERTS_MULTI_PATCHER

        first_keys.push_back(key);
    }

    // Run segmentation on the first keys. This gives us the learned segments we need
    // to program the replacement inner node(s). Each resulting chunk feeds one
    // HostLearnedInnerNode, sized to satisfy segment and child count limits.
    size_t inner_segment_limit = compute_balanced_segment_cap(
        children.size(),
        LEARNED_KEYS_PER_INNER_SEGMENT,
        HOST_TREE_INNER_RUNTIME_LOAD,
        false);
    auto segments = build_pgm_segments_for_keys(first_keys.data(), first_keys.size(),
                                                EPSILON_BUILD_RUNTIME_INNER,
                                                inner_segment_limit == 0 ? LEARNED_KEYS_PER_INNER_SEGMENT
                                                                         : inner_segment_limit);

    // Log segmentation efficiency for this level
    // DOCA_LOG_INFO("Inner segmentation: %zu nodes -> %zu segments (avg %.1f nodes/segment, max %zu)",
    //               first_keys.size(), segments.size(),
    //               segments.empty() ? 0.0 : static_cast<double>(first_keys.size()) / segments.size(),
    //               LEARNED_KEYS_PER_INNER_SEGMENT);

#if DEBUG_INSERTS_MULTI_PATCHER == 1   
    if (segments.empty()) {
        DOCA_LOG_ERR("Failed to build segments while constructing inner subtree");
        return {};
    }
#endif //DEBUG_INSERTS_MULTI_PATCHER

    auto group_sizes = compute_inner_segment_groups(segments, children.size(),
                                                    LEARNED_SEGMENTS_PER_INNER);

#if DEBUG_INSERTS_MULTI_PATCHER == 1   
    if (group_sizes.empty()) {
        DOCA_LOG_ERR("Segment grouping failed while constructing inner subtree");
        return {};
    }
#endif //DEBUG_INSERTS_MULTI_PATCHER

    const size_t max_segments_per_node = LEARNED_SEGMENTS_PER_INNER;
    size_t seg_index = 0;
    for (size_t group_idx = 0; group_idx < group_sizes.size() && seg_index < segments.size();
         ++group_idx) {
        size_t segments_this_node = group_sizes[group_idx];
        size_t seg_end = std::min(seg_index + segments_this_node, segments.size());

        // Each chunk of at most LEARNED_SEGMENTS_PER_INNER segments becomes a new
        // HostLearnedInnerNode. Everything is allocated fresh so callers can treat
        // the returned vector as an immutable snapshot that will later be linked in.
        HostLearnedInnerNode* node = allocate_inner();
#if DEBUG_INSERTS_MULTI_PATCHER == 1 
        if (!node) {
            DOCA_LOG_ERR("Failed to allocate inner node while constructing subtree");
            for (auto* allocated : new_nodes) {
                deallocate_node(allocated);
            }
            return {};
        }
#endif //DEBUG_INSERTS_MULTI_PATCHER

        node->~HostLearnedInnerNode();
        new (node) HostLearnedInnerNode();

        size_t child_start = segments[seg_index].first_position;
        size_t child_end = segments[seg_end - 1].last_position + 1;
        size_t child_count = child_end - child_start;

#if DEBUG_INSERTS_MULTI_PATCHER == 1 
        if (child_end > children.size() || child_start > child_end) {
            DOCA_LOG_ERR("Inner node child range out of bounds while grouping segments");
            deallocate_node(node);
            for (auto* allocated : new_nodes) {
                deallocate_node(allocated);
            }
            return {};
        }
        if (child_count > max_children) {
            DOCA_LOG_ERR("Inner node child count (%zu) exceeds supported maximum", child_count);
            deallocate_node(node);
            for (auto* allocated : new_nodes) {
                deallocate_node(allocated);
            }
            return {};
        }
#endif //DEBUG_INSERTS_MULTI_PATCHER

        // Every replacement inner node inherits the partition supplied by the caller
        // (typically the partition of the leaf that triggered this rebuild).
        node->partition_id = assigned_partition;

        // Copy the relevant children and pad the remainder with nulls so the node is
        // ready to be stitched as-is.
        node->num_children = static_cast<uint16_t>(child_count);
        for (size_t idx = 0; idx < child_count; idx++) {
            node->children[idx] = children[child_start + idx];
        }
        for (size_t idx = child_count; idx < max_children; idx++) {
            node->children[idx] = nullptr;
        }

        // Program learned segments in local coordinates. We reuse compute_local_model
        // to convert from global child indices to offsets inside this node.
        size_t segment_count = seg_end - seg_index;

#if DEBUG_INSERTS_MULTI_PATCHER == 1 
        if (segment_count > max_segments_per_node) {
            DOCA_LOG_ERR("Inner node segment count (%zu) exceeds supported maximum", segment_count);
            deallocate_node(node);
            for (auto* allocated : new_nodes) {
                deallocate_node(allocated);
            }
            return {};
        }
#endif //DEBUG_INSERTS_MULTI_PATCHER
        node->num_segments = static_cast<uint16_t>(segment_count);
        for (size_t s = 0; s < segment_count; s++) {
            const auto& seg = segments[seg_index + s];
            size_t global_child_start = seg.first_position;
            size_t global_child_end = seg.last_position + 1;
            uint64_t segment_first_key = first_keys[global_child_start];
            auto [slope, intercept] = compute_local_model(seg, segment_first_key, global_child_start);
            uint16_t local_first_idx = static_cast<uint16_t>(global_child_start - child_start);
            uint16_t local_child_count = static_cast<uint16_t>(global_child_end - global_child_start);
            node->segments[s] = LearnedSegment(segment_first_key, slope, intercept,
                                               local_first_idx, local_child_count);
        }
        for (size_t s = segment_count; s < LEARNED_SEGMENTS_PER_INNER; s++) {
            node->segments[s] = LearnedSegment();
        }

        node->compute_key_range();
        new_nodes.push_back(node);
        seg_index = seg_end;
    }

    return new_nodes;
}

// Helper function to build PGM segments for keys
std::vector<PLASegment> HostLearnedTree::build_pgm_segments_for_keys(
    const uint64_t* keys, size_t count, int epsilon, size_t max_segment_size) {
    auto key_accessor = [keys](size_t i) { return keys[i]; };
    return build_pgm_segments_common(count, epsilon / 2, max_segment_size, key_accessor);
}

HostLearnedTreeNode* HostLearnedTree::build_tree_recursive(std::vector<HostLearnedTreeNode*>& nodes) {
    if (nodes.empty()) return nullptr;
    if (nodes.size() == 1) return nodes[0];

    DOCA_LOG_INFO("Building inner level from %zu nodes", nodes.size());


    // Step 1: Extract all first keys from nodes at this level
    std::vector<uint64_t> all_first_keys;
    all_first_keys.reserve(nodes.size());

    for (auto* node : nodes) {

        uint64_t first_key = 0;
        if (node->type == HostLearnedTreeNode::LEARNED_LEAF) {
            auto* leaf = static_cast<HostLearnedLeafNode*>(node);
            first_key = leaf->first_key;
        } else {
            auto* inner_node = static_cast<HostLearnedInnerNode*>(node);
            first_key = inner_node->min_key;
        }
        all_first_keys.push_back(first_key);
    }

    // Step 2: Build ALL PGM segments for this level with size constraint
    size_t bulk_inner_segment_limit = compute_balanced_segment_cap(
        all_first_keys.size(),
        LEARNED_KEYS_PER_INNER_SEGMENT,
        HOST_TREE_INNER_BULK_LOAD,
        true);
    auto all_segments = build_pgm_segments_for_keys(all_first_keys.data(), all_first_keys.size(),
                                                    EPSILON_BUILD_BULKLOAD_INNER,
                                                    bulk_inner_segment_limit == 0
                                                        ? LEARNED_KEYS_PER_INNER_SEGMENT
                                                        : bulk_inner_segment_limit);

    // Log segmentation efficiency for this level
    // DOCA_LOG_INFO("Inner segmentation: %zu nodes -> %zu segments (avg %.1f nodes/segment, max %zu)",
    //               nodes.size(), all_segments.size(),
    //               all_segments.empty() ? 0.0 : static_cast<double>(nodes.size()) / all_segments.size(),
    //               LEARNED_KEYS_PER_INNER_SEGMENT);

    // Step 3: Create inner nodes by distributing segments
    auto inner_nodes = create_inner_nodes_from_segments(all_segments, nodes, all_first_keys,
                                                       static_cast<uint16_t>(EPSILON_BUILD_BULKLOAD_INNER));

    DOCA_LOG_INFO("Created %zu inner nodes (avg %.1f segments/node)",
                  inner_nodes.size(),
                  inner_nodes.empty() ? 0.0 : static_cast<double>(all_segments.size()) / inner_nodes.size());


    // Convert to base class pointers for recursion
    std::vector<HostLearnedTreeNode*> next_level(inner_nodes.begin(), inner_nodes.end());
    return build_tree_recursive(next_level);
}

// Assign partitions across the host tree immediately after bulk load. The root
// stays in partition 0; its direct inner children are spread evenly left-to-right,
// and deeper inner nodes simply inherit their parent's partition.
void HostLearnedTree::assign_partitions_for_bulk_root(HostLearnedTreeNode* root) {
    if (!root) {
        return;
    }

    if (root->type == HostLearnedTreeNode::LEARNED_LEAF) {
        root->partition_id = 0;
        return;
    }

    auto* root_inner = static_cast<HostLearnedInnerNode*>(root);
    root_inner->partition_id = 0;

    const size_t partition_count = HOST_TREE_NUM_PARTITIONS ? HOST_TREE_NUM_PARTITIONS : 1;

    std::vector<HostLearnedInnerNode*> first_level;
    first_level.reserve(root_inner->num_children);
    for (uint16_t i = 0; i < root_inner->num_children; i++) {
        HostLearnedTreeNode* child = root_inner->children[i];
        if (child && child->type == HostLearnedTreeNode::LEARNED_INNER) {
            first_level.push_back(static_cast<HostLearnedInnerNode*>(child));
        }
    }

    if (!first_level.empty()) {
        size_t cursor = 0;
        size_t base = first_level.size() / partition_count;
        size_t remainder = first_level.size() % partition_count;

        for (size_t partition = 0; partition < partition_count && cursor < first_level.size(); partition++) {
            size_t assign_count = base + (partition < remainder ? 1 : 0);
            for (size_t idx = 0; idx < assign_count && cursor < first_level.size(); idx++, cursor++) {
                first_level[cursor]->partition_id = static_cast<uint16_t>(partition);
            }
        }

        // Safety: if partition_count was zero (guarded above) or other anomalies, ensure leftovers use last partition.
        while (cursor < first_level.size()) {
            first_level[cursor]->partition_id = static_cast<uint16_t>(partition_count - 1);
            cursor++;
        }
    }

    std::queue<HostLearnedInnerNode*> pending;
    pending.push(root_inner);

    while (!pending.empty()) {
        HostLearnedInnerNode* current = pending.front();
        pending.pop();

        for (uint16_t i = 0; i < current->num_children; i++) {
            HostLearnedTreeNode* child = current->children[i];
            if (!child) {
                continue;
            }
            if (child->type == HostLearnedTreeNode::LEARNED_INNER) {
                auto* inner_child = static_cast<HostLearnedInnerNode*>(child);
                if (current != root_inner) {
                    inner_child->partition_id = current->partition_id;
                }
                pending.push(inner_child);
            } else {
                auto* leaf_child = static_cast<HostLearnedLeafNode*>(child);
                leaf_child->partition_id = current->partition_id;
            }
        }
    }

}

void HostLearnedTree::log_partition_layout() const {
    HostLearnedTreeNode* root = get_root();
    if (!root) {
        DOCA_LOG_INFO("partition layout: <empty tree>");
        return;
    }

    auto build_bfs_string = [](HostLearnedTreeNode* node) -> std::string {
        std::queue<HostLearnedTreeNode*> pending;
        pending.push(node);
        std::string result;

        while (!pending.empty()) {
            HostLearnedTreeNode* current = pending.front();
            pending.pop();

            result += std::to_string(current->partition_id);

            if (current->type != HostLearnedTreeNode::LEARNED_INNER) {
                continue;
            }

            auto* inner = static_cast<HostLearnedInnerNode*>(current);
            for (uint16_t i = 0; i < inner->num_children; ++i) {
                HostLearnedTreeNode* child = inner->children[i];
                if (child) {
                    pending.push(child);
                }
            }
        }

        return result;
    };

    DOCA_LOG_INFO("root partition %u", root->partition_id);

    if (root->type != HostLearnedTreeNode::LEARNED_INNER) {
        return;
    }

    auto* root_inner = static_cast<HostLearnedInnerNode*>(root);
    size_t child_index = 0;

    for (uint16_t child_slot = 0; child_slot < root_inner->num_children; ++child_slot) {
        HostLearnedTreeNode* child = root_inner->children[child_slot];
        if (!child) {
            continue;
        }
        std::string subtree = build_bfs_string(child);
        const char* type_label = child->type == HostLearnedTreeNode::LEARNED_INNER ? "inner" : "leaf";
        DOCA_LOG_INFO("-->> layer one %s [%zu] -->> %s", type_label, child_index, subtree.c_str());
        child_index++;
    }
}

void HostLearnedTree::bulk_load(const std::vector<KVPair>& sorted_data) {

    // Use PGM's optimal segmentation with size constraint
    // Each segment becomes a leaf with size <= LEARNED_KEYS_PER_LEAF
    size_t bulk_segment_limit = compute_balanced_segment_cap(
        sorted_data.size(),
        LEARNED_KEYS_PER_LEAF,
        HOST_TREE_LEAF_BULK_LOAD,
        true);
    auto segments = PLABuilder::build_segments_for_kvpairs(
        sorted_data.data(),
        sorted_data.size(),
        EPSILON_BUILD_BULKLOAD_LEAF,
        bulk_segment_limit == 0 ? LEARNED_KEYS_PER_LEAF : bulk_segment_limit  // Maximum segment size
    );


    // Build leaf nodes from PGM segments
    std::vector<HostLearnedTreeNode*> leaves;
    leaves.reserve(segments.size());

    for (const auto& segment : segments) {
        // Each PGM segment becomes exactly one leaf
        // The segmentation already ensures size <= LEARNED_KEYS_PER_LEAF
        size_t start = segment.first_position;
        size_t end = segment.last_position + 1;

        HostLearnedLeafNode* leaf = create_leaf_from_pgm_segment(sorted_data.data(), start, end, segment,
                                                                EPSILON_BUILD_BULKLOAD_LEAF);
        leaves.push_back(leaf);
    }


    // Build tree bottom-up
    HostLearnedTreeNode* new_root = build_tree_recursive(leaves);
    assign_partitions_for_bulk_root(new_root);
    root_.store(new_root, std::memory_order_release);
}

bool HostLearnedTree::find(uint64_t key, uint64_t& value) const {
    HostLearnedTreeNode* current = root_.load(std::memory_order_acquire);

    while (current->type == HostLearnedTreeNode::LEARNED_INNER) {
        auto* inner = static_cast<HostLearnedInnerNode*>(current);
        current = inner->find_child(key);
    }

    auto* leaf = static_cast<HostLearnedLeafNode*>(current);
    return leaf->find(key, value);
}

HostLearnedLeafNode* HostLearnedTree::find_leaf(uint64_t key) const {

    HostLearnedTreeNode* current = root_.load(std::memory_order_acquire);

    while (current->type == HostLearnedTreeNode::LEARNED_INNER) {
        auto* inner = static_cast<HostLearnedInnerNode*>(current);
        current = inner->find_child(key);
    }

    return static_cast<HostLearnedLeafNode*>(current);
}

size_t HostLearnedTree::get_height() const {
    if (!root_.load(std::memory_order_acquire)) return 0;
    
    size_t height = 1;
    HostLearnedTreeNode* current = root_.load(std::memory_order_acquire);
    
    while (current && current->type == HostLearnedTreeNode::LEARNED_INNER) {
        auto* inner = static_cast<HostLearnedInnerNode*>(current);
        if (inner->num_children > 0) {
            current = inner->children[0];
            height++;
        } else {
            break;
        }
    }
    
    return height;
}

size_t HostLearnedTree::get_leaf_pool_usage() const {
    return leaf_pool_->allocated_chunks() * sizeof(HostLearnedLeafNode);
}

size_t HostLearnedTree::get_inner_pool_usage() const {
    return inner_pool_->allocated_chunks() * sizeof(HostLearnedInnerNode);
}

size_t HostLearnedTree::get_dpa_leaf_pool_usage() const {
    return dpa_allocator_ ? dpa_allocator_->get_leaf_pool_usage() : 0;
}

size_t HostLearnedTree::get_dpa_inner_pool_usage() const {
    return dpa_allocator_ ? dpa_allocator_->get_inner_pool_usage() : 0;
}

// Send complete tree to DPA via stitch commands (for bulk load)
void HostLearnedTree::send_complete_tree_to_dpa() {

    if (!host_global_area_) {
        DOCA_LOG_ERR("send_complete_tree_to_dpa called without host global area");
        return;
    }

    if (!root_.load(std::memory_order_acquire)) {
        DOCA_LOG_WARN("send_complete_tree_to_dpa called with null root");
        return;
    }

    if (!dpa_allocator_) {
        DOCA_LOG_ERR("send_complete_tree_to_dpa requires DPA allocator");
        return;
    }

    // First pass: ensure every node in the tree owns a DPA shadow allocation.
    HostLearnedTreeNode* root = root_.load(std::memory_order_acquire);

    std::queue<HostLearnedTreeNode*> alloc_queue;
    alloc_queue.push(root);

    while (!alloc_queue.empty()) {
        HostLearnedTreeNode* node = alloc_queue.front();
        alloc_queue.pop();

        if (!ensure_dpa_mapping(node)) {
            DOCA_LOG_ERR("send_complete_tree_to_dpa failed to map node %p", node);
            continue;
        }

        if (node->type == HostLearnedTreeNode::LEARNED_INNER) {
            auto* inner = static_cast<HostLearnedInnerNode*>(node);
            for (uint16_t i = 0; i < inner->num_children; i++) {
                if (inner->children[i]) {
                    alloc_queue.push(inner->children[i]);
                }
            }
        }
    }

    // Second pass: reuse the dynamic stitch helpers so bulk load and live updates
    // follow the exact same command format.
    std::queue<HostLearnedTreeNode*> node_queue;
    node_queue.push(root);

    std::vector<HostLearnedTreeNode*> batch;
    batch.reserve(16);

    while (!node_queue.empty()) {
        HostLearnedTreeNode* node = node_queue.front();
        node_queue.pop();

        batch.clear();
        batch.push_back(node);
        emit_copy_stitches(batch);

        if (node->type == HostLearnedTreeNode::LEARNED_INNER) {
            auto* inner = static_cast<HostLearnedInnerNode*>(node);
            for (uint16_t i = 0; i < inner->num_children; i++) {
                if (inner->children[i]) {
                    node_queue.push(inner->children[i]);
                }
            }
        }
    }

    send_root_stitch(root);
}

// Placeholder for dynamic operations - will be implemented with RCU
void HostLearnedTree::process_buffer_patch(HostLearnedLeafNode* leaf, 
                                          const KVPair* buffer_data, size_t buffer_count) {
    // TODO: Implement RCU-based update
}

/*
 * update_leaf merges a patcher's insert buffer into a host leaf and orchestrates any
 * structural changes required to keep the learned tree consistent.
 *
 * The flow is:
 *   1. Merge the insert buffer with the leaf's sorted area to obtain a fresh, sorted view.
 *   2. If the merged keys match the leaf's existing keys, we detected pure updates:
 *        - With USE_MICRO_LEAVES enabled we only change host-side values and emit a stitch
 *          that clears the leaf's update flag (no data copies are required on the DPA).
 *        - Without micro leaves we update the host leaf in place and enqueue a copy stitch
 *          so the DPA leaf is overwritten; no parent or connect stitches are needed.
 *        - In both cases the leaf identity is unchanged and parent inners are untouched.
 *   3. If the merged keys differ, we rebuild the affected subtree:
 *        - The merged entries are segmented; a single segment yields one replacement leaf
 *          that can be pointer-swapped under the direct parent.
 *        - Multiple segments materialise multiple leaves and, if needed, new inner nodes.
 *          Parent nodes are rebuilt (respecting partitioning) and the restructure may
 *          cascade to the root. Copy/connect/root stitches are emitted to reflect the
 *          new materialised subtree on the DPA.
 */
bool HostLearnedTree::update_leaf(HostLearnedLeafNode* leaf,
    const uint64_t* insert_buffer_keys_ptr,
    const uint64_t* insert_buffer_values_ptr,
    size_t insert_buffer_high,
    size_t thread_id) {

#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (!leaf) {
        DOCA_LOG_ERR("update_leaf called with null leaf");
        return false;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER

    size_t clamped_buffer_high = std::min(insert_buffer_high,
                                          static_cast<size_t>(LEARNED_INSERT_BUFFER_SIZE));

#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (clamped_buffer_high > 0 &&
        (!insert_buffer_keys_ptr || !insert_buffer_values_ptr)) {
        DOCA_LOG_ERR("update_leaf called with invalid buffer pointers (leaf=%p)", leaf);
        return false;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER

#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (leaf->type != HostLearnedTreeNode::LEARNED_LEAF) {
        DOCA_LOG_ERR("update_leaf called with non-leaf node %p (type=%u)", leaf,
                     static_cast<unsigned>(leaf->type));
        return false;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER

#if TREE_UPDATE_STATS_FOR_FIRST
    if (update_stats_.completion_update_ns.load(std::memory_order_acquire) != 0) {
        send_unflag_stitch(leaf);
        return true;
    }
#endif

#if TREE_UPDATE_STATS_FOR_FIRST
    if (clamped_buffer_high > 0) {
        if (update_stats_.first_update_ns.load(std::memory_order_relaxed) == 0) {
            uint64_t now_ns = steady_now_ns();
            uint64_t expected_first = 0;
            update_stats_.first_update_ns.compare_exchange_strong(
                expected_first, now_ns, std::memory_order_relaxed);
        }

        uint64_t previous_total =
            update_stats_.elements_processed.fetch_add(
                static_cast<uint64_t>(clamped_buffer_high), std::memory_order_relaxed);
        uint64_t current_total = previous_total + static_cast<uint64_t>(clamped_buffer_high);

        // Estimate additional pending inserts still buffered across all leaves.
        uint64_t pending_estimate = 0;
        uint64_t leaf_count = get_num_leafs();
        if (leaf_count > 0) {
            // Saturating multiply: pending = leaf_count * LEARNED_INSERT_BUFFER_SIZE
            const uint64_t buffer_size = static_cast<uint64_t>(LEARNED_INSERT_BUFFER_SIZE);
            if (leaf_count > std::numeric_limits<uint64_t>::max() / buffer_size) {
                pending_estimate = std::numeric_limits<uint64_t>::max();
            } else {
                pending_estimate = ((double) leaf_count * buffer_size * 0.5f * (37401120.0f / 25000000.0f));
            }
        }

        uint64_t effective_total = current_total;
        // if (effective_total > std::numeric_limits<uint64_t>::max() - pending_estimate) {
        //     effective_total = std::numeric_limits<uint64_t>::max();
        // } else {
        //     effective_total += pending_estimate;
        // }

        if (effective_total >= TREE_UPDATE_STATS_FOR_FIRST) {
            uint64_t end_ns = steady_now_ns();
            uint64_t expected_completion = 0;
            if (update_stats_.completion_update_ns.compare_exchange_strong(
                    expected_completion, end_ns, std::memory_order_relaxed)) {
                update_stats_.completion_elements.store(
                    static_cast<uint64_t>(TREE_UPDATE_STATS_FOR_FIRST),
                    std::memory_order_relaxed);
                print_stats();
            }
        }
    }
#endif

    //#define UPDATE_LEAF_EARLY_OUT_TEST_1
    #ifdef UPDATE_LEAF_EARLY_OUT_TEST_1
    send_unflag_stitch(leaf);
    return true;
    #endif // UPDATE_LEAF_EARLY_OUT_TEST_1

    patches_processed_.fetch_add(1, std::memory_order_relaxed);

    // Start Merging Insert Buffer and Sorted Entries

    KVPair buffer_entries[LEARNED_INSERT_BUFFER_SIZE];
    size_t unique_buffer_count = 0;
    for (size_t i = 0; i < clamped_buffer_high; ++i) {
        uint64_t key = insert_buffer_keys_ptr[i];
        uint64_t value = insert_buffer_values_ptr[i];
        bool found = false;
        for (size_t j = 0; j < unique_buffer_count; ++j) {
            if (buffer_entries[j].key == key) {
                buffer_entries[j].value = value;
                found = true;
                break;
            }
        }
        if (!found) {
            buffer_entries[unique_buffer_count++] = KVPair(key, value);
        }
    }

    std::sort(buffer_entries, buffer_entries + unique_buffer_count,
              [](const KVPair& lhs, const KVPair& rhs) { return lhs.key < rhs.key; });

    KVPair merged_entries[LEARNED_KEYS_PER_LEAF + LEARNED_INSERT_BUFFER_SIZE];
    size_t merged_count = 0;
    size_t leaf_idx = 0;
    size_t buffer_idx = 0;

    while (leaf_idx < leaf->sorted_count && buffer_idx < unique_buffer_count) {
        uint64_t leaf_key = leaf->sorted_keys[leaf_idx];
        uint64_t buffer_key = buffer_entries[buffer_idx].key;
        if (leaf_key < buffer_key) {
            merged_entries[merged_count++] = KVPair(leaf_key, leaf->sorted_values[leaf_idx]);
            leaf_idx++;
        } else if (buffer_key < leaf_key) {
            merged_entries[merged_count++] = buffer_entries[buffer_idx++];
        } else {
            merged_entries[merged_count++] = KVPair(leaf_key, buffer_entries[buffer_idx].value);
            leaf_idx++;
            buffer_idx++;
        }
    }

    while (leaf_idx < leaf->sorted_count) {
        merged_entries[merged_count++] =
            KVPair(leaf->sorted_keys[leaf_idx], leaf->sorted_values[leaf_idx]);
        leaf_idx++;
    }

    while (buffer_idx < unique_buffer_count) {
        merged_entries[merged_count++] = buffer_entries[buffer_idx++];
    }

    // Done merging.

    //#define UPDATE_LEAF_EARLY_OUT_TEST_2
    #ifdef UPDATE_LEAF_EARLY_OUT_TEST_2
    send_unflag_stitch(leaf);
    return true;
    #endif // UPDATE_LEAF_EARLY_OUT_TEST_2

#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (merged_count == 0) {
        DOCA_LOG_WARN("update_leaf produced empty merged set for leaf %p", leaf);
        return false;
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER

    bool keys_match_old = merged_count == leaf->sorted_count;
    if (keys_match_old) {
        for (size_t i = 0; i < merged_count; ++i) {
            if (merged_entries[i].key != leaf->sorted_keys[i]) {
                keys_match_old = false;
                break;
            }
        }
    }

    // Outcomment for raw Insert tests.

    if (keys_match_old) {
        leaf->set_sorted_data(merged_entries, merged_count);
        leaf->buffer_count = 0;

#ifdef USE_MICRO_LEAVES
        uint32_t queue_id = queue_for_leaf(leaf);
        send_unflag_stitch_on_queue(leaf, queue_id);
        //DOCA_LOG_INFO("KEYS MATCH");
#else
        uint32_t queue_id = queue_for_leaf(leaf);
        std::vector<HostLearnedTreeNode*> nodes{leaf};
        std::vector<PreparedCopyStitchCommand> commands;
        prepare_copy_stitch_commands(queue_id, nodes, false, commands);
        emit_copy_stitches_on_queue(queue_id, commands);
#endif
        return true;
    }

    //DOCA_LOG_WARN("Keys dont match, leaf %p", leaf);

    //#define UPDATE_LEAF_KEYS_DONT_MATCH_DEBUG
    #ifdef UPDATE_LEAF_KEYS_DONT_MATCH_DEBUG
    auto log_key_mismatch = [&](const char* reason) {
        std::ostringstream oss;
        oss << "[update_leaf] keys diverged for leaf " << leaf
            << " reason=" << reason
            << " old_count=" << leaf->sorted_count
            << " merged_count=" << merged_count
            << " buffer_high=" << clamped_buffer_high
            << " first_old=" << (leaf->sorted_count ? leaf->sorted_keys[0] : 0)
            << " first_new=" << merged_entries[0].key
            << " last_old=" << (leaf->sorted_count ? leaf->sorted_keys[leaf->sorted_count - 1] : 0)
            << " last_new=" << merged_entries[merged_count - 1].key;
        for (size_t i = 0; i < merged_count; ++i) {
            if (i >= leaf->sorted_count || merged_entries[i].key != leaf->sorted_keys[i]) {
                oss << " mismatch_at=" << i
                    << " old_key=" << (i < leaf->sorted_count ? leaf->sorted_keys[i] : 0)
                    << " new_key=" << merged_entries[i].key;
                break;
            }
        }
        oss << " insert_buffer=[";
        for (size_t i = 0; i < clamped_buffer_high; ++i) {
            if (i > 0) {
                oss << ' ';
            }
            oss << '(' << insert_buffer_keys_ptr[i] << ',' << insert_buffer_values_ptr[i] << ')';
        }
        oss << ']';
        DOCA_LOG_INFO("%s", oss.str().c_str());
    };
    log_key_mismatch("keys_changed");
    #endif // UPDATE_LEAF_KEYS_DONT_MATCH_DEBUG

    // STATE HERE: KEYS DO NOT MATCH. We have real inserts. Now: PGM-Segment the merged entries. 

    //#define UPDATE_LEAF_EARLY_OUT_TEST_3
    #ifdef UPDATE_LEAF_EARLY_OUT_TEST_3
    send_unflag_stitch(leaf);
    return true;
    #endif // UPDATE_LEAF_EARLY_OUT_TEST_3

    struct PendingConnect {
        HostLearnedInnerNode* parent;
        HostLearnedTreeNode* child;
        uint16_t child_index;
    };

    std::vector<HostLearnedTreeNode*> pending_copy_nodes;
    std::vector<PreparedCopyStitchCommand> pending_copy_commands;
    std::vector<PendingConnect> pending_connects;
    HostLearnedTreeNode* pending_root_update = nullptr;
    bool root_update_pending = false;
    const uint32_t patch_queue_id = queue_for_partition(leaf->partition_id);

    std::vector<HostLearnedTreeNode*> new_nodes_for_cleanup;
    std::vector<HostLearnedInnerNode*> held_locks;
    std::vector<HostLearnedTreeNode*> nodes_to_free;
    nodes_to_free.push_back(leaf);

    auto release_locked_nodes = [&]() {
        for (auto it = held_locks.rbegin(); it != held_locks.rend(); ++it) {
            if (*it) {
                (*it)->node_lock.unlock();
            }
        }
        held_locks.clear();
    };

    auto cleanup_new_nodes = [&]() {
        for (auto* node : new_nodes_for_cleanup) {
            deallocate_node(node);
        }
        new_nodes_for_cleanup.clear();
    };

    auto fail_and_cleanup = [&]() -> bool {
        release_locked_nodes();
        cleanup_new_nodes();
        DOCA_LOG_ERR("Fail and cleanup.");
        return false;
    };

    uint64_t new_total_entries = 0;

    //#define UPDATE_LEAF_EARLY_OUT_TEST_4
    #ifdef UPDATE_LEAF_EARLY_OUT_TEST_4
    send_unflag_stitch(leaf);
    return true;
    #endif // UPDATE_LEAF_EARLY_OUT_TEST_4

    size_t segment_limit = compute_balanced_segment_cap(
        merged_count,
        LEARNED_KEYS_PER_LEAF,
        HOST_TREE_LEAF_RUNTIME_LOAD,
        false);

    //#define UPDATE_LEAF_DEBUB_DUPLICATE_LEAF_SEGMENTS
    #ifdef UPDATE_LEAF_DEBUB_DUPLICATE_LEAF_SEGMENTS

    auto dump_merged_entries = [&]() {
        for (size_t dump_idx = 0; dump_idx < merged_count; ++dump_idx) {
            DOCA_LOG_ERR("  merged[%zu]: key=%lu value=%lu",
                         dump_idx,
                         static_cast<unsigned long>(merged_entries[dump_idx].key),
                         static_cast<unsigned long>(merged_entries[dump_idx].value));
        }
    };

    for (size_t idx = 1; idx < merged_count; ++idx) {
        uint64_t prev_key = merged_entries[idx - 1].key;
        uint64_t current_key = merged_entries[idx].key;
        if (current_key < prev_key) {
            DOCA_LOG_ERR("Merged entries are not in ascending order while rebuilding leaf %p (index %zu key %lu < prev %lu)",
                         leaf,
                         idx,
                         static_cast<unsigned long>(current_key),
                         static_cast<unsigned long>(prev_key));
            dump_merged_entries();
            return fail_and_cleanup();
        }
        if (current_key == prev_key) {
            DOCA_LOG_ERR("Merged entries contain duplicate keys while rebuilding leaf %p (indices %zu/%zu key %lu)",
                         leaf,
                         idx - 1,
                         idx,
                         static_cast<unsigned long>(current_key));
            dump_merged_entries();
            return fail_and_cleanup();
        }
    }

    #endif // UPDATE_LEAF_DEBUB_DUPLICATE_LEAF_SEGMENTS

    auto segments = PLABuilder::build_segments_for_kvpairs(
        merged_entries,
        merged_count,
        EPSILON_BUILD_RUNTIME_LEAF,
        segment_limit == 0 ? LEARNED_KEYS_PER_LEAF : segment_limit);

#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (segments.empty()) {
        DOCA_LOG_ERR("PLABuilder returned no segments for non-empty merge (leaf=%p)", leaf);
        return fail_and_cleanup();
    }
#endif // DEBUG_INSERTS_MULTI_PATCHER

    //#define UPDATE_LEAF_EARLY_OUT_TEST_5
    #ifdef UPDATE_LEAF_EARLY_OUT_TEST_5
    send_unflag_stitch(leaf);
    return true;
    #endif // UPDATE_LEAF_EARLY_OUT_TEST_5
    // HERE SLOWDOWN FROM LAST EARLY OUT TEST. 1 thread 6Mops to 1.2Mops.
    // Multithreading helps: 16 patchers: 12.2 Mops

    std::vector<HostLearnedLeafNode*> new_leaves;
    new_leaves.reserve(segments.size());

    for (const auto& segment : segments) {
        size_t start = segment.first_position;
        size_t end = segment.last_position + 1;
#if DEBUG_INSERTS_MULTI_PATCHER == 1
        if (end <= start || end > merged_count) {
            DOCA_LOG_ERR("Invalid segment bounds while updating %p (segment=[%zu,%zu) total=%zu)",
                         leaf, start, end, merged_count);
            return fail_and_cleanup();
        }
#endif // DEBUG_INSERTS_MULTI_PATCHER

        HostLearnedLeafNode* new_leaf = allocate_leaf();
#if DEBUG_INSERTS_MULTI_PATCHER == 1
        if (!new_leaf) {
            DOCA_LOG_ERR("Failed to allocate replacement leaf while updating %p", leaf);
            return fail_and_cleanup();
        }
#endif // DEBUG_INSERTS_MULTI_PATCHER

        new_leaf->partition_id = leaf->partition_id;
        new_leaf->set_sorted_data(merged_entries + start, end - start);
        new_leaf->buffer_count = 0;

        if (end - start > 1) {
            uint64_t segment_first_key = merged_entries[start].key;
            auto [slope, intercept] = compute_local_model(segment, segment_first_key, start);
            new_leaf->set_model(slope, intercept);
        } else {
            new_leaf->set_model(0.0, 0.0);
        }

        new_total_entries += new_leaf->sorted_count;
        new_leaves.push_back(new_leaf);
        pending_copy_nodes.push_back(new_leaf);
        new_nodes_for_cleanup.push_back(new_leaf);
    }

    std::vector<HostLearnedTreeNode*> replacement_children(new_leaves.begin(), new_leaves.end());
    HostLearnedTreeNode* replace_target = leaf;

    //#define UPDATE_LEAF_EARLY_OUT_TEST_6
    #ifdef UPDATE_LEAF_EARLY_OUT_TEST_6
    send_unflag_stitch(leaf);
    return true;
    #endif // 

    /*
     * Restructure strategy:
     *   1. Walk up the tree starting from the original leaf being replaced.
     *   2. If we encounter a parent and there is exactly one replacement child, we simply
     *      swap the pointer in that parent and enqueue a connect stitch – no inner rebuild.
     *   3. Otherwise we splice the replacement children into the parent, materialise the
     *      necessary inner node(s), and continue the process upward with the parent as
     *      the next replace_target.
     *   4. When we reach the root (no parent), we install the replacement subtree as the
     *      new root and emit the appropriate root stitch.
     */

    root_lock_.lock();

    uint64_t this_structural_modification;

    bool restructure_succeeded = false;
    while (!restructure_succeeded) {
        //bool target_node_was_root = false;
#if DEBUG_INSERTS_MULTI_PATCHER == 1
        DOCA_LOG_INFO("Before get_and_lock_parent. id: %u", (unsigned int) thread_id);
#endif // DEBUG_INSERTS_MULTI_PATCHER

        HostLearnedInnerNode* parent = get_and_lock_parent(replace_target);

#if DEBUG_INSERTS_MULTI_PATCHER == 1
        DOCA_LOG_INFO("After get_and_lock_parent. id: %u", (unsigned int) thread_id);
#endif // DEBUG_INSERTS_MULTI_PATCHER

        if (!parent) {
            // if (!target_node_was_root) {
            //     DOCA_LOG_ERR("Failed to locate parent while updating leaf %p (target %p)",
            //                  leaf,
            //                  replace_target);
            //     std::this_thread::sleep_for(std::chrono::seconds(10));
            //     return fail_and_cleanup();
            // }

            nodes_to_free.push_back(replace_target);

            std::vector<HostLearnedTreeNode*> level_nodes(replacement_children.begin(),
                                                          replacement_children.end());

#if DEBUG_INSERTS_MULTI_PATCHER == 1
            if (level_nodes.empty()) {
                DOCA_LOG_ERR("Replacement chain is empty while updating leaf %p", leaf);
                return fail_and_cleanup();
            }
#endif // DEBUG_INSERTS_MULTI_PATCHER

            HostLearnedTreeNode* current_root = root_.load(std::memory_order_acquire);
            bool old_root_was_inner = current_root &&
                                      current_root->type == HostLearnedTreeNode::LEARNED_INNER;
            uint16_t root_partition = leaf->partition_id;

            if (current_root) {
                nodes_to_free.push_back(current_root);
            }

            const size_t partition_count =
                HOST_TREE_NUM_PARTITIONS ? HOST_TREE_NUM_PARTITIONS : 1;

            auto build_inner_level =
                [&](const std::vector<HostLearnedTreeNode*>& children,
                    uint16_t partition,
                    bool inherit_partitions) -> std::vector<HostLearnedTreeNode*> {
                auto new_inners =
                    make_inner_nodes_for_children(children, partition, inherit_partitions);
                if (new_inners.empty()) {
                    return {};
                }

                std::vector<HostLearnedTreeNode*> result;
                result.reserve(new_inners.size());
                for (auto* inner : new_inners) {
                    if (!inner) {
                        continue;
                    }
                    inner->result_from_root_split = 1;
                    pending_copy_nodes.push_back(inner);
                    new_nodes_for_cleanup.push_back(inner);
                    result.push_back(inner);
                }
                return result;
            };

            auto rebuild_root_with_partitions =
                [&](const std::vector<HostLearnedTreeNode*>& initial_nodes,
                    HostLearnedTreeNode*& out_root) -> bool {
                auto flatten_children =
                    [](const std::vector<HostLearnedTreeNode*>& nodes) {
                        std::vector<HostLearnedTreeNode*> children;
                        for (auto* node : nodes) {
                            if (!node) {
                                continue;
                            }
                            if (node->type == HostLearnedTreeNode::LEARNED_INNER) {
                                auto* inner = static_cast<HostLearnedInnerNode*>(node);
                                for (uint16_t idx = 0; idx < inner->num_children; ++idx) {
                                    HostLearnedTreeNode* child = inner->children[idx];
                                    if (child) {
                                        children.push_back(child);
                                    }
                                }
                            } else {
                                children.push_back(node);
                            }
                        }
                        return children;
                    };

                std::vector<HostLearnedTreeNode*> flattened_children =
                    flatten_children(initial_nodes);
                if (flattened_children.empty()) {
                    DOCA_LOG_ERR("Root rebuild has no children after flatten while updating leaf %p",
                                 leaf);
                    return false;
                }

                std::unordered_set<HostLearnedTreeNode*> discard_set(initial_nodes.begin(),
                                                                     initial_nodes.end());
                auto remove_from_container =
                    [&](std::vector<HostLearnedTreeNode*>& container) {
                        if (container.empty()) {
                            return;
                        }
                        container.erase(std::remove_if(container.begin(),
                                                       container.end(),
                                                       [&](HostLearnedTreeNode* candidate) {
                                                           return discard_set.find(candidate) !=
                                                                  discard_set.end();
                                                       }),
                                        container.end());
                    };

                remove_from_container(pending_copy_nodes);
                remove_from_container(new_nodes_for_cleanup);
                for (auto* stale_node : initial_nodes) {
                    deallocate_node(stale_node);
                }

                struct PartitionRun {
                    uint16_t partition;
                    std::vector<HostLearnedTreeNode*> children;
                };

                std::vector<PartitionRun> partition_runs;
                partition_runs.reserve(flattened_children.size());

                std::vector<uint16_t> seen_partitions;
                seen_partitions.reserve(partition_count);

                for (auto* child : flattened_children) {
                    if (!child) {
                        continue;
                    }
                    uint16_t child_partition = child->partition_id;
                    if (partition_runs.empty() ||
                        partition_runs.back().partition != child_partition) {
                        if (std::find(seen_partitions.begin(),
                                      seen_partitions.end(),
                                      child_partition) != seen_partitions.end()) {
                            DOCA_LOG_ERR("Partition %u appears in multiple non-contiguous runs "
                                         "while rebuilding root",
                                         child_partition);
                            return false;
                        }
                        partition_runs.push_back({child_partition, {}});
                        seen_partitions.push_back(child_partition);
                    }
                    partition_runs.back().children.push_back(child);
                }

                if (partition_runs.empty()) {
                    DOCA_LOG_ERR("Failed to derive partition runs while rebuilding root");
                    return false;
                }

                // Enforce the expectation that every partition remains represented.
                if (partition_count > 1 && partition_runs.size() < partition_count) {
                    DOCA_LOG_ERR(
                        "Root rebuild discovered only %zu partition runs (expected at least %zu)",
                        partition_runs.size(),
                        partition_count);
                    return false;
                }

                std::vector<HostLearnedTreeNode*> partition_roots;
                partition_roots.reserve(partition_runs.size());

                for (const auto& run : partition_runs) {
                    std::vector<HostLearnedTreeNode*> level = run.children;
                    if (level.empty()) {
                        DOCA_LOG_ERR("Partition %u has no children during root rebuild",
                                     run.partition);
                        return false;
                    }

                    while (level.size() > 1) {
                        auto next_level = build_inner_level(level,
                                                            run.partition,
                                                            /*inherit_partitions=*/true);
                        if (next_level.empty()) {
                            DOCA_LOG_ERR("Failed to rebuild partition %u while updating leaf %p",
                                         run.partition,
                                         leaf);
                            return false;
                        }
                        if (next_level.size() >= level.size()) {
                            DOCA_LOG_ERR(
                                "Partition %u rebuild did not reduce node count (prev=%zu new=%zu)",
                                run.partition,
                                level.size(),
                                next_level.size());
                            return false;
                        }
                        level.assign(next_level.begin(), next_level.end());
                    }

                    HostLearnedTreeNode* top = level.front();
                    if (!top) {
                        DOCA_LOG_ERR("Null partition root encountered for partition %u",
                                     run.partition);
                        return false;
                    }
                    top->partition_id = run.partition;
                    partition_roots.push_back(top);
                }

                if (partition_count > 1 && partition_roots.size() < partition_count) {
                    DOCA_LOG_ERR("Partition root count %zu smaller than expected partitions %zu",
                                 partition_roots.size(),
                                 partition_count);
                    return false;
                }

                std::vector<HostLearnedTreeNode*> root_level(partition_roots.begin(),
                                                             partition_roots.end());

                while (root_level.size() > 1) {
                    auto next_level = build_inner_level(root_level,
                                                        root_partition,
                                                        /*inherit_partitions=*/false);
                    if (next_level.empty()) {
                        DOCA_LOG_ERR("Failed to fold partition roots into new root while "
                                     "updating leaf %p",
                                     leaf);
                        return false;
                    }
                    if (next_level.size() >= root_level.size()) {
                        DOCA_LOG_ERR("Root folding did not reduce node count (prev=%zu new=%zu)",
                                     root_level.size(),
                                     next_level.size());
                        return false;
                    }
                    root_level.assign(next_level.begin(), next_level.end());
                }

                if (root_level.empty()) {
                    DOCA_LOG_ERR("Partition folding produced no root candidate while updating leaf %p",
                                 leaf);
                    return false;
                }

                out_root = root_level.front();
                return out_root != nullptr;
            };

            HostLearnedTreeNode* new_root = nullptr;
            if (level_nodes.size() == 1) {
                new_root = level_nodes.front();
            } else {
                if (!rebuild_root_with_partitions(level_nodes, new_root)) {
                    return fail_and_cleanup();
                }
            }

            if (!new_root) {
                DOCA_LOG_ERR("Root rebuild failed to produce a new root while updating leaf %p",
                             leaf);
                return fail_and_cleanup();
            }

            if (new_root && new_root->type == HostLearnedTreeNode::LEARNED_INNER) {
                auto* new_root_inner = static_cast<HostLearnedInnerNode*>(new_root);
                new_root_inner->partition_id = root_partition;
                if (!old_root_was_inner) {
                    assign_partitions_for_bulk_root(new_root_inner);
                }
            }

            //std::atomic_thread_fence(std::memory_order_seq_cst);
#if DEBUG_INSERTS_MULTI_PATCHER == 1
            DOCA_LOG_INFO("structural_modifications_began before fetch_add: %u,  id: %u", (unsigned int) this->structural_modifications_began.load(), (unsigned int) thread_id);
#endif // DEBUG_INSERTS_MULTI_PATCHER            
            this_structural_modification = this->structural_modifications_began.fetch_add(1, std::memory_order_acq_rel);
#if DEBUG_INSERTS_MULTI_PATCHER == 1
            DOCA_LOG_INFO("structural_modifications_began after fetch_add: %u,  id: %u", (unsigned int) this->structural_modifications_began.load(), (unsigned int) thread_id);
#endif // DEBUG_INSERTS_MULTI_PATCHER            
            root_.store(new_root, std::memory_order_release);
            //std::atomic_thread_fence(std::memory_order_seq_cst);

            pending_root_update = new_root;
            root_update_pending = true;

            restructure_succeeded = true;
            break;
        }

        //#define UPDATE_LEAF_EARLY_OUT_TEST_7
        #ifdef UPDATE_LEAF_EARLY_OUT_TEST_7
        send_unflag_stitch(leaf);
        break;
        #endif // UPDATE_LEAF_EARLY_OUT_TEST_7

        HostLearnedTreeNode* swapped_child = nullptr;
        bool parent_handled = false;
        if (replacement_children.size() == 1) {
            HostLearnedTreeNode* new_child = replacement_children.front();
            bool swapped = false;
            uint16_t child_index = 0;
            for (uint16_t i = 0; i < parent->num_children; ++i) {
                if (parent->children[i] == replace_target) {
                    //std::atomic_thread_fence(std::memory_order_seq_cst);
#if DEBUG_INSERTS_MULTI_PATCHER == 1
                    DOCA_LOG_INFO("structural_modifications_began before fetch_add: %u,  id: %u", (unsigned int) this->structural_modifications_began.load(), (unsigned int) thread_id);
#endif // DEBUG_INSERTS_MULTI_PATCHER                     
                    this_structural_modification = this->structural_modifications_began.fetch_add(1, std::memory_order_acq_rel);
#if DEBUG_INSERTS_MULTI_PATCHER == 1                    
                    DOCA_LOG_INFO("structural_modifications_began after fetch_add: %u,  id: %u", (unsigned int) this->structural_modifications_began.load(), (unsigned int) thread_id);
#endif // DEBUG_INSERTS_MULTI_PATCHER                     
                    parent->children[i] = new_child;
                    //std::atomic_thread_fence(std::memory_order_seq_cst);

                    swapped_child = new_child;
                    swapped = true;
                    child_index = i;
                    break;
                }
            }

            if (swapped) {
                nodes_to_free.push_back(replace_target);
                pending_connects.push_back({parent, new_child, child_index});
                restructure_succeeded = true;
                parent_handled = true;
            } else {
                DOCA_LOG_ERR("Not swapped while updating leaf %p", leaf);
                std::this_thread::sleep_for(std::chrono::seconds(10));
            }
        }

        //#define UPDATE_LEAF_EARLY_OUT_TEST_8
        #ifdef UPDATE_LEAF_EARLY_OUT_TEST_8
        send_unflag_stitch(leaf);
        break;
        #endif // UPDATE_LEAF_EARLY_OUT_TEST_8

        if (parent_handled) {
#if DEBUG_INSERTS_MULTI_PATCHER == 1
            // ADD DEBUG BLOCK
            auto compute_child_first_key =
                [](HostLearnedTreeNode* node, uint64_t& out_key) -> bool {
                if (!node) {
                    return false;
                }
                if (node->type == HostLearnedTreeNode::LEARNED_LEAF) {
                    auto* leaf = static_cast<HostLearnedLeafNode*>(node);
                    if (leaf->sorted_count > 0) {
                        out_key = leaf->first_key;
                        return true;
                    }
                    if (leaf->buffer_count > 0) {
                        uint64_t min_key = leaf->insert_buffer_keys[0];
                        for (uint16_t i = 1; i < leaf->buffer_count; ++i) {
                            min_key = std::min(min_key, leaf->insert_buffer_keys[i]);
                        }
                        out_key = min_key;
                        return true;
                    }
                    return false;
                }

                auto* inner = static_cast<HostLearnedInnerNode*>(node);
                if (inner->num_children > 0) {
                    out_key = inner->min_key;
                    return true;
                }
                return false;
            };

            bool order_violation = false;
            uint64_t previous_key = 0;
            uint16_t previous_index = 0;
            HostLearnedTreeNode* previous_child = nullptr;
            bool have_previous = false;

            for (uint16_t idx = 0; idx < parent->num_children; ++idx) {
                HostLearnedTreeNode* child = parent->children[idx];
                if (!child) {
                    continue;
                }

                uint64_t child_key = 0;
                if (!compute_child_first_key(child, child_key)) {
                    continue;
                }

                if (have_previous && child_key < previous_key) {
                    order_violation = true;
                    DOCA_LOG_ERR(
                        "Parent %p partition=%u child order violation after swap "
                        "(replace_target=%p new_child=%p): child[%u]=%p first_key=%lu < "
                        "child[%u]=%p first_key=%lu",
                        parent,
                        parent->partition_id,
                        replace_target,
                        swapped_child,
                        idx,
                        child,
                        static_cast<unsigned long>(child_key),
                        previous_index,
                        previous_child,
                        static_cast<unsigned long>(previous_key));
                    break;
                }

                have_previous = true;
                previous_key = child_key;
                previous_child = child;
                previous_index = idx;
            }

            if (order_violation) {
                for (uint16_t idx = 0; idx < parent->num_children; ++idx) {
                    HostLearnedTreeNode* child = parent->children[idx];
                    if (!child) {
                        continue;
                    }
                    uint64_t child_key = 0;
                    bool has_key = compute_child_first_key(child, child_key);
                    std::ostringstream oss;
                    oss << "    child[" << idx << "]=" << child
                        << " type="
                        << (child->type == HostLearnedTreeNode::LEARNED_LEAF ? "leaf" : "inner");
                    if (has_key) {
                        oss << " first_key=" << child_key;
                    } else {
                        oss << " first_key=<n/a>";
                    }
                    if (child->type == HostLearnedTreeNode::LEARNED_LEAF) {
                        auto* leaf = static_cast<HostLearnedLeafNode*>(child);
                        oss << " sorted_count=" << leaf->sorted_count
                            << " buffer_count=" << leaf->buffer_count;
                    } else {
                        auto* inner = static_cast<HostLearnedInnerNode*>(child);
                        oss << " num_children=" << inner->num_children;
                    }
                    DOCA_LOG_ERR("%s", oss.str().c_str());
                }
            }
            // END DEBUG BLOCK
#endif // DEBUG_INSERTS_MULTI_PATCHER    
            // parent->node_lock.unlock();
            // held_locks.pop_back();
            held_locks.push_back(parent);
            break;
        }

        //#define UPDATE_LEAF_EARLY_OUT_TEST_9
        #ifdef UPDATE_LEAF_EARLY_OUT_TEST_9
        send_unflag_stitch(leaf);
        break;
        #endif // UPDATE_LEAF_EARLY_OUT_TEST_9

        std::vector<HostLearnedTreeNode*> updated_children;
        updated_children.reserve(parent->num_children - 1 + replacement_children.size());

        // DO WE INTRODUCE DUPLICATES HERE? 
        bool replaced = false;
        for (uint16_t i = 0; i < parent->num_children; ++i) {
            HostLearnedTreeNode* child = parent->children[i];
            if (child == replace_target) {
                updated_children.insert(updated_children.end(),
                                        replacement_children.begin(),
                                        replacement_children.end());
                replaced = true;
            } else {
                if (child)
                    updated_children.push_back(child);
            }
        }

#if DEBUG_INSERTS_MULTI_PATCHER == 1
        if (!replaced) {
            DOCA_LOG_ERR("Parent %p did not reference target %p during update", parent, replace_target);
            std::this_thread::sleep_for(std::chrono::seconds(10));
            parent->node_lock.unlock();
            held_locks.pop_back();
            return fail_and_cleanup();
        }
#endif // DEBUG_INSERTS_MULTI_PATCHER    

#if DEBUG_INSERTS_MULTI_PATCHER == 1 
        // START DEBUG BLOCK

        auto compute_child_first_key = [](HostLearnedTreeNode* node, uint64_t& out_key) -> bool {
            if (!node) {
                return false;
            }
            if (node->type == HostLearnedTreeNode::LEARNED_LEAF) {
                auto* leaf = static_cast<HostLearnedLeafNode*>(node);
                if (leaf->sorted_count > 0) {
                    out_key = leaf->first_key;
                    return true;
                }
                if (leaf->buffer_count > 0) {
                    uint64_t min_key = leaf->insert_buffer_keys[0];
                    for (uint16_t i = 1; i < leaf->buffer_count; ++i) {
                        min_key = std::min(min_key, leaf->insert_buffer_keys[i]);
                    }
                    out_key = min_key;
                    return true;
                }
                return false;
            }

            auto* inner = static_cast<HostLearnedInnerNode*>(node);
            if (inner->num_children > 0) {
                out_key = inner->min_key;
                return true;
            }
            return false;
        };

        bool updated_children_violation = false;
        uint64_t prev_child_key = 0;
        HostLearnedTreeNode* prev_child_ptr = nullptr;
        size_t prev_child_index = 0;
        bool have_prev_child = false;

        for (size_t idx = 0; idx < updated_children.size(); ++idx) {
            HostLearnedTreeNode* child = updated_children[idx];
            if (!child) {
                continue;
            }
            uint64_t child_key = 0;
            if (!compute_child_first_key(child, child_key)) {
                continue;
            }
            if (have_prev_child && child_key < prev_child_key) {
                updated_children_violation = true;
                DOCA_LOG_ERR(
                    "Parent %p partition=%u updated_children order violation before rebuild "
                    "(replace_target=%p): updated_children[%zu]=%p first_key=%lu < updated_children[%zu]=%p first_key=%lu",
                    parent,
                    parent->partition_id,
                    replace_target,
                    idx,
                    child,
                    static_cast<unsigned long>(child_key),
                    prev_child_index,
                    prev_child_ptr,
                    static_cast<unsigned long>(prev_child_key));
                break;
            }

            have_prev_child = true;
            prev_child_key = child_key;
            prev_child_ptr = child;
            prev_child_index = idx;
        }

        if (updated_children_violation) {
            for (size_t idx = 0; idx < updated_children.size(); ++idx) {
                HostLearnedTreeNode* child = updated_children[idx];
                if (!child) {
                    continue;
                }
                uint64_t child_key = 0;
                bool has_key = compute_child_first_key(child, child_key);
                std::ostringstream oss;
                oss << "    updated_children[" << idx << "]=" << child
                    << " type="
                    << (child->type == HostLearnedTreeNode::LEARNED_LEAF ? "leaf" : "inner");
                if (has_key) {
                    oss << " first_key=" << child_key;
                } else {
                    oss << " first_key=<n/a>";
                }
                if (child->type == HostLearnedTreeNode::LEARNED_LEAF) {
                    auto* leaf = static_cast<HostLearnedLeafNode*>(child);
                    oss << " sorted_count=" << leaf->sorted_count
                        << " buffer_count=" << leaf->buffer_count;
                } else {
                    auto* inner = static_cast<HostLearnedInnerNode*>(child);
                    oss << " num_children=" << inner->num_children;
                }
                DOCA_LOG_ERR("%s", oss.str().c_str());
            }
        }

        // END DEBUG BLOCK
#endif // DEBUG_INSERTS_MULTI_PATCHER  

        //#define UPDATE_LEAF_EARLY_OUT_TEST_10
        #ifdef UPDATE_LEAF_EARLY_OUT_TEST_10
        send_unflag_stitch(leaf);
        break;
        #endif // UPDATE_LEAF_EARLY_OUT_TEST_10

        auto new_parent_nodes = make_inner_nodes_for_children(
            updated_children,
            leaf->partition_id,
            /*inherit_partition_from_children=*/false);

        //#define UPDATE_LEAF_EARLY_OUT_TEST_11
        #ifdef UPDATE_LEAF_EARLY_OUT_TEST_11
        send_unflag_stitch(leaf);
        break;
        #endif // UPDATE_LEAF_EARLY_OUT_TEST_11

#if DEBUG_INSERTS_MULTI_PATCHER == 1 
        // SANITY CHECK BEGIN
        if (!new_parent_nodes.empty()) {
            uint64_t old_min_key = std::numeric_limits<uint64_t>::max();
            for (uint16_t idx = 0; idx < parent->num_children; ++idx) {
                HostLearnedTreeNode* child = parent->children[idx];
                if (!child) {
                    continue;
                }
                uint64_t child_min = 0;
                if (child->type == HostLearnedTreeNode::LEARNED_LEAF) {
                    auto* leaf_child = static_cast<HostLearnedLeafNode*>(child);
                    if (leaf_child->sorted_count == 0) {
                        continue;
                    }
                    child_min = leaf_child->first_key;
                } else {
                    auto* inner_child = static_cast<HostLearnedInnerNode*>(child);
                    child_min = inner_child->min_key;
                }
                old_min_key = std::min(old_min_key, child_min);
            }
            if (old_min_key == std::numeric_limits<uint64_t>::max()) {
                old_min_key = 0;
            }

            uint64_t new_min_key = std::numeric_limits<uint64_t>::max();
            size_t new_child_count = 0;
            HostLearnedTreeNode* new_last_child = nullptr;
            for (auto* new_parent : new_parent_nodes) {
                if (!new_parent) {
                    continue;
                }
                auto* new_inner = static_cast<HostLearnedInnerNode*>(new_parent);
                for (uint16_t idx = 0; idx < new_inner->num_children; ++idx) {
                    HostLearnedTreeNode* child = new_inner->children[idx];
                    if (!child) {
                        continue;
                    }
                    uint64_t child_min = 0;
                    if (child->type == HostLearnedTreeNode::LEARNED_LEAF) {
                        auto* leaf_child = static_cast<HostLearnedLeafNode*>(child);
                        if (leaf_child->sorted_count == 0) {
                            continue;
                        }
                        child_min = leaf_child->first_key;
                    } else {
                        auto* inner_child = static_cast<HostLearnedInnerNode*>(child);
                        child_min = inner_child->min_key;
                    }
                    new_min_key = std::min(new_min_key, child_min);
                    new_last_child = child;
                    ++new_child_count;
                }
            }
            if (new_min_key == std::numeric_limits<uint64_t>::max()) {
                new_min_key = 0;
            }

            HostLearnedTreeNode* old_last_child =
                parent->num_children ? parent->children[parent->num_children - 1] : nullptr;

            bool range_matches = (old_min_key == new_min_key) &&
                                 (new_child_count == updated_children.size());
            if (!range_matches) {
                DOCA_LOG_ERR("Rebuilt parent range while updating parent=%p (old_min=%lu new_min=%lu children_before=%u children_after=%zu)",
                             parent,
                             static_cast<unsigned long>(old_min_key),
                             static_cast<unsigned long>(new_min_key),
                             static_cast<unsigned>(parent->num_children),
                             new_child_count);
                DOCA_LOG_ERR("  replace_target=%p replacement_children=%zu updated_children=%zu new_parent_nodes=%zu",
                             replace_target,
                             replacement_children.size(),
                             updated_children.size(),
                             new_parent_nodes.size());
                DOCA_LOG_ERR("  old_last_child=%p new_last_child=%p",
                             old_last_child,
                             new_last_child);
            }
        }
        // SANITY CHECK END
#endif // DEBUG_INSERTS_MULTI_PATCHER  

#if DEBUG_INSERTS_MULTI_PATCHER == 1 
        if (new_parent_nodes.empty()) {
            DOCA_LOG_ERR("Failed to rebuild parent %p while updating leaf %p", parent, leaf);
            std::this_thread::sleep_for(std::chrono::seconds(10));
            parent->node_lock.unlock();
            held_locks.pop_back();
            return fail_and_cleanup();
        }
#endif // DEBUG_INSERTS_MULTI_PATCHER  

        pending_copy_nodes.insert(pending_copy_nodes.end(),
                                  new_parent_nodes.begin(),
                                  new_parent_nodes.end());
        for (auto* node : new_parent_nodes) {
            new_nodes_for_cleanup.push_back(node);
        }

        nodes_to_free.push_back(parent);

        replacement_children.assign(new_parent_nodes.begin(), new_parent_nodes.end());
        replace_target = parent;
    }

    if (!restructure_succeeded) {
        return fail_and_cleanup();
    }

    {
        //SpinLockGuard guard(stats_lock_);
        if (num_entries_ >= leaf->sorted_count) {
            num_entries_ -= leaf->sorted_count;
        } else {
            DOCA_LOG_WARN("num_entries_ underflow guard triggered while updating leaf %p (num_entries_=%lu, old_count=%u)",
                          leaf,
                          static_cast<unsigned long>(num_entries_),
                          static_cast<unsigned>(leaf->sorted_count));
            num_entries_ = 0;
        }
        num_entries_ += new_total_entries;
    }

#if DEBUG_INSERTS_MULTI_PATCHER == 1 
    DOCA_LOG_INFO("Before end-lock. id: %u", (unsigned int) thread_id);
#endif // DEBUG_INSERTS_MULTI_PATCHER  

    // Here: under parent lock, prepare copy stitch data, so that we after releasing the node, do not incoporate new data into the stitches, that are not yet copied.
    if (!pending_copy_nodes.empty()) {
        prepare_copy_stitch_commands(patch_queue_id, pending_copy_nodes, true, pending_copy_commands);
    }

    release_locked_nodes();

    root_lock_.unlock();

#if DEBUG_INSERTS_MULTI_PATCHER == 1 
    DOCA_LOG_INFO("structural_modifications_enqueued before wait: %u (wait for %u),  id: %u", (unsigned int) this->structural_modifications_enqueued.load(), this_structural_modification, (unsigned int) thread_id);
#endif // DEBUG_INSERTS_MULTI_PATCHER  

    while (this_structural_modification != structural_modifications_enqueued.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    //root_lock_.lock();

    const uint32_t queue_id = patch_queue_id;

#if DEBUG_INSERTS_MULTI_PATCHER == 1 
    DOCA_LOG_INFO("Before stitching. id: %u", (unsigned int) thread_id);
#endif // DEBUG_INSERTS_MULTI_PATCHER  

    if (!pending_copy_commands.empty()) {
        emit_copy_stitches_on_queue(queue_id, pending_copy_commands, true);
    }

#if DEBUG_INSERTS_MULTI_PATCHER == 1 
    DOCA_LOG_INFO("Between copy-connect stitching. id: %u", (unsigned int) thread_id);
#endif // DEBUG_INSERTS_MULTI_PATCHER  

    for (const auto& op : pending_connects) {
        send_connect_stitch_on_queue(op.parent, op.child, op.child_index, queue_id);
    }

#if DEBUG_INSERTS_MULTI_PATCHER == 1 
    DOCA_LOG_INFO("Between connect-root stitching. id: %u", (unsigned int) thread_id);
#endif // DEBUG_INSERTS_MULTI_PATCHER  

    if (root_update_pending && pending_root_update) {
        send_root_stitch_on_queue(pending_root_update, queue_id);
    }

#if DEBUG_INSERTS_MULTI_PATCHER == 1 
    DOCA_LOG_INFO("After stitching. id: %u", (unsigned int) thread_id);
#endif // DEBUG_INSERTS_MULTI_PATCHER  

    // DEADLOCK + Segfault
    //release_locked_nodes();

    std::unordered_set<HostLearnedTreeNode*> released;
    released.reserve(nodes_to_free.size());
    for (auto* old_node : nodes_to_free) {
        if (!old_node || released.find(old_node) != released.end()) {
            continue;
        }
        released.insert(old_node);
        deallocate_node(old_node);
    }

#if DEBUG_INSERTS_MULTI_PATCHER == 1 
    DOCA_LOG_INFO("After dealloc. id: %u", (unsigned int) thread_id);
#endif // DEBUG_INSERTS_MULTI_PATCHER  

    // DEADLOCK or broken
    //release_locked_nodes();

    structural_modifications_enqueued.fetch_add(1, std::memory_order_release);

    //root_lock_.unlock();
    //DOCA_LOG_INFO("After end-lock. id: %u", (unsigned int) thread_id);

#if DEBUG_INSERTS_MULTI_PATCHER == 1 
    DOCA_LOG_INFO("After all. id: %u", (unsigned int) thread_id);
#endif // DEBUG_INSERTS_MULTI_PATCHER  

    return true;
}

/*
 * Finds the parent of `node_ptr` while ensuring the parent is locked before returning.
 * Outline:
 *   1. Begin at the root and attempt to lock the current node; if locking fails, restart from the root.
 *   2. When the lock succeeds, identify the child that should contain `node_ptr`.
 *   3. If the child matches `node_ptr`, keep the current lock and return; otherwise release the lock,
 *      descend to the child, and continue.
 *   4. If the traversal cannot progress (missing child or contended lock), release the lock and retry
 *      from the root with the latest topology.
 */
HostLearnedInnerNode* HostLearnedTree::get_and_lock_parent(HostLearnedTreeNode* node_ptr) {
    HostLearnedTreeNode* current_root = root_.load(std::memory_order_acquire);
    if (!node_ptr || node_ptr == current_root) {
        return nullptr;
    }

    uint64_t search_key = 0;
    if (node_ptr->type == HostLearnedTreeNode::LEARNED_LEAF) {
        auto* leaf_node = static_cast<HostLearnedLeafNode*>(node_ptr);
        if (leaf_node->sorted_count == 0) {
            return nullptr;
        }
        search_key = leaf_node->first_key;
    } else {
        auto* inner_node = static_cast<HostLearnedInnerNode*>(node_ptr);
        search_key = inner_node->min_key;
    }

    while (true) {
        //root_lock_.lock();
        HostLearnedTreeNode* root = root_.load(std::memory_order_acquire);
        if (!root || root == node_ptr || root->type != HostLearnedTreeNode::LEARNED_INNER) {
            return nullptr;
        }

        auto* current = static_cast<HostLearnedInnerNode*>(root);
        if (!current->node_lock.try_lock()) {
            //root_lock_.unlock();
            std::this_thread::yield();
            continue;
        }

        while (true) {
            HostLearnedTreeNode* child = current->find_child(search_key);
            if (!child) {
                //root_lock_.unlock();
                current->node_lock.unlock();
                std::this_thread::yield();
                break;  // Retry from latest root pointer
            }

            if (child == node_ptr) {
                //root_lock_.unlock();
                return current;  // Caller owns the lock for modification
            }

            if (child->type != HostLearnedTreeNode::LEARNED_INNER) {
                current->node_lock.unlock();
                return nullptr;
            }

            auto* next = static_cast<HostLearnedInnerNode*>(child);
            if (next->node_lock.try_lock()) {
                current->node_lock.unlock();
                current = next;
                continue;
            }

            //root_lock_.unlock();
            current->node_lock.unlock();
            std::this_thread::yield();
            break;  // Contended child, restart from root
        }
    }
}

void HostLearnedTree::print_stats() const {
#if TREE_UPDATE_STATS_FOR_FIRST
    const uint64_t start_ns =
        update_stats_.first_update_ns.load(std::memory_order_relaxed);
    const uint64_t end_ns =
        update_stats_.completion_update_ns.load(std::memory_order_relaxed);
    const uint64_t completion_total =
        update_stats_.completion_elements.load(std::memory_order_relaxed);
    if (start_ns == 0 || end_ns <= start_ns) {
        return;
    }
    if (completion_total == 0) {
        return;
    }

    const double duration_seconds =
        static_cast<double>(end_ns - start_ns) / 1e9;
    if (duration_seconds <= 0.0) {
        return;
    }

    const double operations = static_cast<double>(completion_total);
    const double mops = (operations / duration_seconds) / 1e6;

    DOCA_LOG_INFO("HostLearnedTree processed %lu inserts in %.6f s (%.3f Mops)",
                  static_cast<unsigned long>(completion_total),
                  duration_seconds,
                  mops);
#else
    (void)update_stats_;
#endif
}
