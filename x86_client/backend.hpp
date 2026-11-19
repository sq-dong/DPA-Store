#pragma once

#include "config.hpp"
#include "target_interface.hpp"

#include <cstddef>
#include <optional>
#include <string>

/* Runs the default threaded backend loop that manages outstanding operations per thread. */
void backend_default_thread_execute(std::size_t thread_id);

/* Load a single trace file shared across threads; returns number of operations. */
std::size_t load_traces(const std::string &trace_file,
                        std::optional<OperationType> override_type = std::nullopt);
std::size_t load_loop_trace(const std::string &trace_file,
                            std::optional<OperationType> override_type = std::nullopt);

/* Deterministic per-thread access without shared counters. Stride is controlled by set_operation_rank_span. */
Operation get_operation(std::size_t index, std::size_t rank);
void set_operation_rank_span(std::size_t span);
void set_manual_rank(std::size_t rank, std::size_t world_size);
std::size_t global_rank();
std::size_t global_world_size();
std::optional<Operation> get_main_operation(std::size_t index, std::size_t rank, bool wrap = false);
std::optional<Operation> get_loop_operation(std::size_t index, std::size_t rank);
void start_barrier(const std::string& barrier_file);
