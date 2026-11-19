#include "backend.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static std::vector<Operation> g_trace;
static std::vector<Operation> g_trace_loop;

namespace {
std::atomic<std::uint64_t> g_operation_counter{0};
std::atomic<std::size_t> g_rank_span{1};
std::atomic<std::size_t> g_rank{0};
std::atomic<std::size_t> g_world_size{1};

struct OutstandingSlot {
    bool active{false};
    Operation operation{};
};

bool any_pending(const std::vector<OutstandingSlot>& slots) {
    for (const auto& slot : slots) {
        if (slot.active) {
            return true;
        }
    }
    return false;
}
}  // namespace

std::vector<Operation> load_trace_file(const std::string &path,
                                       std::optional<OperationType> override_type) {
    size_t num_lines{0};
    std::ifstream file{path};
    std::string line;
    std::vector<Operation> ops;

    /* Test file, throw exc if bad. */
    if (file.bad())
        throw std::ios_base::failure("Failed to open file at " + path + ".");

    while (std::getline(file, line)) {
        num_lines++;

        std::istringstream iss(line);
        std::string op;
        uint64_t key{0};
        uint64_t val{0};

        if (!(iss >> op)) {
            continue;
        }

        OperationType type{};
        bool valid{true};

        if (override_type.has_value()) {
            if (!(iss >> key)) {
                valid = false;
            } else {
                type = *override_type;
                val = key * 2;
            }
        } else if (op == "GET" || op == "READ") {
            type = OperationType::Get;
            iss >> key;
        } else if (op == "INSERT" || op == "UPDATE" || op == "PUT") {
            type = OperationType::Insert;
            iss >> key >> val;
        } else if (op == "DELETE" || op == "DEL") {
            type = OperationType::Delete;
            iss >> key;
        } else if (op == "SCAN" || op == "RANGE") {
            type = OperationType::Range;
            iss >> key >> val;
        } else if (op == "PING") {
            type = OperationType::Ping;
        } else {
            valid = false;
        }

        if (!valid) {
            continue;
        }

        const auto &o = ops.emplace_back(num_lines, type, key, val);
    }

    return ops;
}

std::size_t load_traces(const std::string &trace_file, std::optional<OperationType> override_type) {
    std::size_t count{0};
    try {
        if (trace_file.empty()) {
            std::cerr << "Trace file path is empty." << std::endl;
            g_trace.clear();
            return 0;
        }
        g_trace = load_trace_file(trace_file, override_type);
        g_operation_counter.store(0, std::memory_order_relaxed);
        std::cout << "Loaded trace file: " << trace_file
                  << " (" << g_trace.size() << " entries)." << std::endl;
        count = g_trace.size();
    } catch (const std::ios_base::failure& e) {
        std::cerr << "Failed to open file at " << trace_file
                  << " with error " << e.what() << std::endl;
        g_trace.clear();
        count = 0;
    } catch (...) {
        std::cerr << "Failed to open file at " << trace_file
                  << " with unknown error " << std::endl;
        g_trace.clear();
        count = 0;
    }

    return count;
}

std::size_t load_loop_trace(const std::string &trace_file, std::optional<OperationType> override_type) {
    std::size_t count{0};
    try {
        if (trace_file.empty()) {
            std::cerr << "Loop trace file path is empty." << std::endl;
            g_trace_loop.clear();
            return 0;
        }
        g_trace_loop = load_trace_file(trace_file, override_type);
        std::cout << "Loaded loop trace file: " << trace_file
                  << " (" << g_trace_loop.size() << " entries)." << std::endl;
        count = g_trace_loop.size();
    } catch (const std::ios_base::failure& e) {
        std::cerr << "Failed to open loop file at " << trace_file
                  << " with error " << e.what() << std::endl;
        g_trace_loop.clear();
        count = 0;
    } catch (...) {
        std::cerr << "Failed to open loop file at " << trace_file
                  << " with unknown error " << std::endl;
        g_trace_loop.clear();
        count = 0;
    }

    return count;
}

Operation get_next_operation() {
    if (g_trace.empty()) {
        return {};
    }

    const auto id = g_operation_counter.fetch_add(1, std::memory_order_relaxed);
    auto op = g_trace[id % g_trace.size()];
    op.id = id;
    return op;
}

Operation get_operation(std::size_t index, std::size_t rank) {
    if (g_trace.empty()) {
        return {};
    }

    const std::size_t span = std::max<std::size_t>(g_rank_span.load(std::memory_order_relaxed), 1);
    const std::size_t global_id = rank + index * span;
    auto op = g_trace[global_id % g_trace.size()];
    op.id = global_id;
    return op;
}

void set_operation_rank_span(std::size_t span) {
    if (span == 0) {
        span = 1;
    }
    g_rank_span.store(span, std::memory_order_relaxed);
}

void backend_default_thread_execute(std::size_t thread_id) {
    const std::size_t max_operations = static_cast<std::size_t>(OPERATIONS_PER_THREAD);
    if (max_operations == 0) {
        return;
    }

    const std::size_t slot_count = static_cast<std::size_t>(OUTSTANDING_OPS_PER_THREAD);
    if (slot_count == 0) {
        return;
    }

    std::vector<OutstandingSlot> slots(slot_count);
    std::size_t current_slot = 0;
    std::size_t issued = 0;
    std::size_t completed = 0;
    bool has_pending = false;

    auto advance_slot = [&]() {
        current_slot = (current_slot + 1) % slot_count;
    };

    while (completed < max_operations) {
        bool attempted_issue = false;

        if (issued < max_operations) {
            auto& slot = slots[current_slot];
            if (!slot.active) {
                Operation op = get_next_operation();
                ++issued;
                const OperationResult result =
                    target_do_operation(thread_id, current_slot, op);

                if (result.status == OperationStatus::Pending) {
                    slot.active = true;
                    slot.operation = op;
                    has_pending = true;
                    advance_slot();
                } else {
                    ++completed;
                    // Slot remains inactive, reuse immediately as requested.
                }
                attempted_issue = true;
            } else {
                advance_slot();
            }
        }

        if (has_pending) {
            auto freed_slots = target_check_for_replies(thread_id);
            bool freed_any = false;
            for (std::size_t bucket_id : freed_slots) {
                if (bucket_id < slot_count && slots[bucket_id].active) {
                    slots[bucket_id].active = false;
                    ++completed;
                    freed_any = true;
                }
            }
            has_pending = any_pending(slots);

            if (!freed_any && has_pending) {
                std::this_thread::yield();
            }
        } else if (!attempted_issue) {
            // Nothing to issue and nothing pending; avoid tight loop.
            break;
        }
    }

    // Drain outstanding operations, if any remain.
    while (has_pending) {
        auto freed_slots = target_check_for_replies(thread_id);
        bool freed_any = false;
        for (std::size_t bucket_id : freed_slots) {
            if (bucket_id < slot_count && slots[bucket_id].active) {
                slots[bucket_id].active = false;
                    ++completed;
                freed_any = true;
            }
        }
        has_pending = any_pending(slots);
        if (!freed_any && has_pending) {
            std::this_thread::yield();
        }
    }
}

void set_manual_rank(std::size_t rank, std::size_t world_size) {
    if (world_size == 0) {
        world_size = 1;
    }
    g_rank.store(rank, std::memory_order_relaxed);
    g_world_size.store(world_size, std::memory_order_relaxed);
}

std::size_t global_rank() {
    return g_rank.load(std::memory_order_relaxed);
}

std::size_t global_world_size() {
    return g_world_size.load(std::memory_order_relaxed);
}

std::optional<Operation> get_main_operation(std::size_t index, std::size_t rank, bool wrap) {
    const std::size_t span = std::max<std::size_t>(g_rank_span.load(std::memory_order_relaxed), 1);
    if (g_trace.empty()) {
        return std::nullopt;
    }
    const std::size_t global_id = rank + index * span;
    const std::size_t trace_size = g_trace.size();

    if (!wrap && global_id >= trace_size) {
        return std::nullopt;
    }

    auto op = g_trace[wrap ? (global_id % trace_size) : global_id];
    op.id = global_id;
    return op;
}

std::optional<Operation> get_loop_operation(std::size_t index, std::size_t rank) {
    const std::size_t span = std::max<std::size_t>(g_rank_span.load(std::memory_order_relaxed), 1);
    if (g_trace_loop.empty()) {
        return std::nullopt;
    }
    const std::size_t global_id = rank + index * span;
    const std::size_t idx = global_id % g_trace_loop.size();
    auto op = g_trace_loop[idx];
    op.id = global_id;
    return op;
}

void start_barrier(const std::string& barrier_file) {
    if (barrier_file.empty()) {
        return;
    }
    const auto path = std::filesystem::path(barrier_file);
    while (!std::filesystem::exists(path)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
