#include "backend.hpp"
#include "target_interface.hpp"
#include "config.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <iostream>
#include <optional>
#include <tuple>
#include <memory>
#include <string>
#include <vector>

#include <gflags/gflags.h>

#include "benchs/load_config.hh"
#include "benchs/load_data.hh"

#include "r2/src/libroutine.hh"
#include "r2/src/thread.hh"

#include "rlib/core/common.hh"
#include "rlib/core/nicinfo.hh"
#include "rlib/core/rctrl.hh"

#include "rolex/lib.hh"
#include "rolex/trait.hpp"

#include "xcomm/src/rpc/mod.hh"
#include "xcomm/src/transport/rdma_ud_t.hh"
#include "xcomm/tests/transport_util.hh"
#include "xutils/local_barrier.hh"

DEFINE_uint32(dst_port_base, ROLEX_SERVER_PORT, "Base port of server.");
// CONFIGURE FOR YOUR SITE: lab default is oscar (137.226.133.237); override
// at runtime with -dst_ip=<server>.
DEFINE_string(dst_ip, "137.226.133.237", "Server IP.");

DEFINE_string(main_trace_path, "", "Main trace file path (no wrap unless --trace_wrap_main).");
DEFINE_string(loop_trace_path, "", "Loop trace file path (wrap). Defaults to main trace.");
DEFINE_string(main_trace_op, "", "Override operation for main trace (GET/READ/INSERT/UPDATE/DELETE/RANGE/SCAN/PUT/PING).");
DEFINE_string(loop_trace_op, "", "Override operation for loop trace (GET/READ/INSERT/UPDATE/DELETE/RANGE/SCAN/PUT/PING).");
DEFINE_uint32(main_one_of, 20, "Issue one main-trace op every N operations. 0 disables main trace.");
DEFINE_bool(trace_wrap_main, false, "Wrap main trace when exhausted.");
DEFINE_uint64(leaf_num, ROLEX_REGISTERED_LEAF_COUNT, "The number of registed leaves.");
DEFINE_uint32(nic_index, ROLEX_NIC_INDEX, "Index of the NIC interface to use.");
DEFINE_uint32(nthreads, ROLEX_THREAD_COUNT, "Number of threads.");
DEFINE_uint32(num_coroutines, ROLEX_COROUTINE_COUNT, "Number of co-routines.");
DEFINE_uint32(ops_per_thread, 0, "TODO");
DEFINE_uint32(latency_queue_id, 0, "Queue index to sample latencies from.");

DEFINE_uint32(rank_id, 0, "Global rank of this process (pass from launcher).");
DEFINE_uint32(world_size, 1, "Global world size (pass from launcher).");

using namespace rolex;

namespace {

using RPC = rpc::RPCCore<transport::UDTransport,
                          transport::UDRecvTransport<ROLEX_TRANSPORT_REPLY_BYTES>,
                          transport::UDSessionManager<ROLEX_TRANSPORT_REPLY_BYTES>>;

struct ThreadState {
    uint64_t loop_idx{0};
    uint64_t main_idx{0};
    uint32_t main_stride_counter{0};
    bool main_exhausted{false};
    uint64_t issued{0};
    std::size_t op_rank{0};
    uint64_t completed{0};
    bool start_recorded{false};
    bool stop_posted{false};
    std::chrono::steady_clock::time_point start_time{};
    std::chrono::steady_clock::time_point end_time{};
};

struct RolexContext {
    std::size_t thread_count{0};
    std::size_t coroutine_count{0};
    std::size_t operations_per_thread{0};
    std::atomic<bool> running{false};
    std::atomic<bool> scheduler_stopped{false};
    std::atomic<std::size_t> threads_completed{0};
    std::unique_ptr<xstore::util::PBarrier> barrier;
    std::vector<std::unique_ptr<r2::Thread<usize>>> workers;
    std::vector<ThreadState> thread_states;
    std::vector<uint64_t> latency_samples;  // Only for latency_queue_id.
    uint64_t timer_hz{0};
};

RolexContext g_ctx;

void configure_rolex_flags_from_macros() {
    FLAGS_leaf_num = ROLEX_REGISTERED_LEAF_COUNT;
    FLAGS_nic_index = ROLEX_NIC_INDEX;
    FLAGS_nthreads = static_cast<std::uint32_t>(ROLEX_THREAD_COUNT);
    FLAGS_num_coroutines = static_cast<std::uint32_t>(ROLEX_COROUTINE_COUNT);

    FLAGS_nkeys = ROLEX_BENCHMARK_NUM_KEYS;
    FLAGS_non_nkeys = ROLEX_BENCHMARK_NUM_NON_KEYS;
    FLAGS_workloads = ROLEX_BENCHMARK_WORKLOAD;
    FLAGS_mem_threads = ROLEX_BENCHMARK_MEM_THREADS;
    FLAGS_threads = ROLEX_BENCHMARK_SERVER_THREADS;
    FLAGS_coros = static_cast<std::int32_t>(ROLEX_COROUTINE_COUNT);
    FLAGS_read_ratio = ROLEX_BENCHMARK_READ_RATIO;
    FLAGS_insert_ratio = ROLEX_BENCHMARK_INSERT_RATIO;
    FLAGS_update_ratio = ROLEX_BENCHMARK_UPDATE_RATIO;
}

std::optional<OperationType> parse_override_op(const std::string &op_str) {
    if (op_str.empty()) {
        return std::nullopt;
    }

    std::string upper;
    upper.reserve(op_str.size());
    for (char c : op_str) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }

    if (upper == "GET" || upper == "READ") {
        return OperationType::Get;
    }
    if (upper == "INSERT" || upper == "UPDATE" || upper == "PUT") {
        return OperationType::Insert;
    }
    if (upper == "DELETE" || upper == "DEL") {
        return OperationType::Delete;
    }
    if (upper == "RANGE" || upper == "SCAN") {
        return OperationType::Range;
    }
    if (upper == "PING") {
        return OperationType::Ping;
    }

    LOG(1) << "[rolex-target] Unknown operation override '" << op_str
           << "' (expected GET/READ/INSERT/UPDATE/PUT/DELETE/RANGE/SCAN/PING)";
    return std::nullopt;
}

void print_latencies() {
    if (g_ctx.latency_samples.empty()) {
        printf("No latency samples captured.\n");
        return;
    }
    std::vector<uint64_t> sorted = g_ctx.latency_samples;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double q) {
        const double pos = q * (sorted.size() - 1);
        const size_t idx = static_cast<size_t>(std::ceil(pos));
        return sorted[idx];
    };
    printf(
        "Rank %zu latency percentiles (ns): p50=%lu p90=%lu p99=%lu p99.9=%lu p99.99=%lu "
        "p99.999=%lu p99.9999=%lu\n",
        global_rank(), pct(0.50), pct(0.90), pct(0.99), pct(0.999), pct(0.9999), pct(0.99999),
        pct(0.999999));
}

auto remote_search(const rolex::KeyType& key,
                   RPC& rpc,
                   transport::UDTransport& sender,
                   R2_ASYNC) -> ::r2::Option<ValType> {
    char send_buf[64];
    char reply_buf[sizeof(ReplyValue)];

    rpc::RPCOp op;

    op.set_msg(MemBlock(send_buf, sizeof(send_buf)))
        .set_req()
        .set_rpc_id(GET)
        .set_corid(R2_COR_ID())
        .add_one_reply(rpc.reply_station, {reply_buf, sizeof(ReplyValue)})
        .add_arg<rolex::KeyType>(key);

    ASSERT(rpc.reply_station.cor_ready(R2_COR_ID()) == false);
    auto ret = op.execute_w_key(&sender, 0);
    ASSERT(ret == IOCode::Ok);

    R2_PAUSE_AND_YIELD;

    ReplyValue r = *(reinterpret_cast<ReplyValue*>(reply_buf));

    if (r.status) {
        return static_cast<ValType>(r.val);
    }

    return {};
}

void remote_put(const rolex::KeyType& key,
                const ValType& val,
                RPC& rpc,
                transport::UDTransport& sender,
                R2_ASYNC) {
    std::string data;
    data += ::xstore::util::Marshal<rolex::KeyType>::serialize_to(key);
    data += ::xstore::util::Marshal<ValType>::serialize_to(val);

    char send_buf[64];
    char reply_buf[sizeof(ReplyValue)];

    rpc::RPCOp op;
    op.set_msg(MemBlock(send_buf, sizeof(send_buf)))
        .set_req()
        .set_rpc_id(PUT)
        .set_corid(R2_COR_ID())
        .add_one_reply(rpc.reply_station, {reply_buf, sizeof(ReplyValue)})
        .add_opaque(data);

    ASSERT(op.execute_w_key(&sender, 0) == IOCode::Ok);

    R2_PAUSE_AND_YIELD;
}

void remote_remove(const rolex::KeyType& key,
                   RPC& rpc,
                   transport::UDTransport& sender,
                   R2_ASYNC) {
    std::string data;
    data += ::xstore::util::Marshal<rolex::KeyType>::serialize_to(key);

    char send_buf[64];
    char reply_buf[sizeof(ReplyValue)];

    rpc::RPCOp op;
    op.set_msg(MemBlock(send_buf, sizeof(send_buf)))
        .set_req()
        .set_rpc_id(DELETE)
        .set_corid(R2_COR_ID())
        .add_one_reply(rpc.reply_station, {reply_buf, sizeof(ReplyValue)})
        .add_opaque(data);

    ASSERT(op.execute_w_key(&sender, 0) == IOCode::Ok);

    R2_PAUSE_AND_YIELD;
}

void remote_scan(const rolex::KeyType& key,
                 const u64& n,
                 RPC& rpc,
                 transport::UDTransport& sender,
                 R2_ASYNC) {
    std::string data;
    data += ::xstore::util::Marshal<rolex::KeyType>::serialize_to(key);
    data += ::xstore::util::Marshal<u64>::serialize_to(n);

    char send_buf[64];
    char reply_buf[sizeof(ReplyValue)];

    rpc::RPCOp op;
    op.set_msg(MemBlock(send_buf, sizeof(send_buf)))
        .set_req()
        .set_rpc_id(SCAN)
        .set_corid(R2_COR_ID())
        .add_one_reply(rpc.reply_station, {reply_buf, sizeof(ReplyValue)})
        .add_opaque(data);

    ASSERT(op.execute_w_key(&sender, 0) == IOCode::Ok);

    R2_PAUSE_AND_YIELD;
}

bool fetch_operation(Operation& operation, size_t thread_id) {
    if (!g_ctx.running.load(std::memory_order_acquire)) {
        return false;
    }
    if (thread_id >= g_ctx.thread_states.size()) {
        return false;
    }

    auto &state = g_ctx.thread_states[thread_id];
    const bool wrap_main = FLAGS_trace_wrap_main;

    while (g_ctx.running.load(std::memory_order_acquire)) {
        if (g_ctx.operations_per_thread > 0 && state.issued >= g_ctx.operations_per_thread) {
            return false;
        }

        bool use_loop = true;
        if (FLAGS_main_one_of > 0 && !(state.main_exhausted && !wrap_main)) {
            if (state.main_stride_counter + 1 >= FLAGS_main_one_of) {
                use_loop = false;
                state.main_stride_counter = 0;
            } else {
                state.main_stride_counter++;
            }
        }

        std::optional<Operation> op_opt;
        if (use_loop) {
            op_opt = get_loop_operation(state.loop_idx, state.op_rank);
            state.loop_idx++;
            if (!op_opt) {
                g_ctx.running.store(false, std::memory_order_release);
                return false;
            }
        } else {
            if (!wrap_main && state.main_exhausted) {
                continue;
            }
            op_opt = get_main_operation(state.main_idx, state.op_rank, wrap_main);
            state.main_idx++;
            if (!op_opt) {
                state.main_exhausted = true;
                continue;
            }
            state.main_exhausted = false;
        }

        operation = *op_opt;
        operation.id = state.issued;
        state.issued++;
        return true;
    }

    return false;
}

bool execute_operation(const Operation& operation,
                       RPC& rpc,
                       transport::UDTransport& sender,
                       R2_ASYNC) {
    const auto key = static_cast<rolex::KeyType>(operation.key);
    const auto value = static_cast<ValType>(operation.value);

    switch (operation.type) {
        case OperationType::Get:
            remote_search(key, rpc, sender, R2_ASYNC_WAIT);
            return true;
        case OperationType::Insert:
            remote_put(key, value, rpc, sender, R2_ASYNC_WAIT);
            return true;
        case OperationType::Delete:
            remote_remove(key, rpc, sender, R2_ASYNC_WAIT);
            return true;
        case OperationType::Range:
            remote_scan(key,
                        static_cast<u64>(operation.len),
                        rpc,
                        sender,
                        R2_ASYNC_WAIT);
            return true;
        case OperationType::Ping:
            return true;
        default:
            LOG(1) << "[rolex-target] unknown operation type: "
                   << static_cast<int>(operation.type);
            return false;
    }
}

usize run_worker(std::size_t thread_id) {
    auto server_addr = FLAGS_dst_ip + ":" + std::to_string(FLAGS_dst_port_base);

    auto nic_for_sender = RNic::create(RNicInfo::query_dev_names().at(FLAGS_nic_index)).value();
    auto ud_qp = UD::create(nic_for_sender, QPConfig()).value();

    auto mem_region = HugeRegion::create(ROLEX_THREAD_MEMORY_BYTES).value();
    auto mem = mem_region->convert_to_rmem().value();
    auto handler = RegHandler::create(mem, nic_for_sender).value();

    test::SimpleAllocator alloc(mem, handler->get_reg_attr().value());
    auto recv_rs_at_send =
        RecvEntriesFactory<test::SimpleAllocator, ROLEX_TRANSPORT_REPLY_BYTES, 1024>::create(alloc);

    RDMA_ASSERT(ud_qp->post_recvs(*recv_rs_at_send, ROLEX_TRANSPORT_INITIAL_RECVS) == IOCode::Ok);

    transport::UDTransport sender;

    r2::Timer connect_timer;
    do {
        std::string param = "b" + std::to_string(static_cast<int>(thread_id));
        if (sender.connect(server_addr, param, thread_id, ud_qp) == IOCode::Ok) {
            LOG(2) << "[rolex-target] thread " << thread_id
                   << " connected to " << server_addr;
            break;
        }

        LOG(2) << "[rolex-target] thread " << thread_id
               << " failed to connect to " << server_addr
               << " with param " << param;

        if (connect_timer.passed_sec() >= ROLEX_CONNECTION_RETRY_SECONDS) {
            ASSERT(false) << "connection failed at thread #" << thread_id;
        }
    } while (connect_timer.passed_sec() < ROLEX_CONNECTION_RETRY_SECONDS);

    LOG(2) << "Num operations per thread: " << g_ctx.operations_per_thread;

    RPC rpc(g_ctx.coroutine_count + 1);
    auto send_buf = std::get<0>(alloc.alloc_one(ROLEX_TRANSPORT_BUFFER_BYTES).value());
    ASSERT(send_buf != nullptr);
    auto lkey = handler->get_reg_attr().value().key;
    std::memset(send_buf, 0, ROLEX_TRANSPORT_BUFFER_BYTES);

    auto conn_op =
        rpc::RPCOp::get_connect_op(MemBlock(send_buf, ROLEX_TRANSPORT_REPLY_BYTES),
                                   sender.get_connect_data().value());
    ASSERT(conn_op.execute_w_key(&sender, lkey) == IOCode::Ok);

    rpc::UDRecvTransport<ROLEX_TRANSPORT_REPLY_BYTES> recv_s(ud_qp, recv_rs_at_send);

    SScheduler ssched;
    rpc.reg_poll_future(ssched, &recv_s);

    g_ctx.barrier->wait();

    for (std::size_t i = 0; i < g_ctx.coroutine_count; ++i) {
        ssched.spawn([thread_id, &rpc, &sender](R2_ASYNC) {
            std::uint64_t local_completed = 0;
            uint64_t before_ns;

            while (true) {
                if (!g_ctx.running.load(std::memory_order_acquire)) {
                    break;
                }

                auto& state = g_ctx.thread_states[thread_id];
                if (!state.start_recorded) {
                    state.start_recorded = true;
                    state.start_time = std::chrono::steady_clock::now();
                }

                Operation operation{};
                if (!fetch_operation(operation, thread_id)) {
                    break;
                }

                if (thread_id == FLAGS_latency_queue_id) {
                    before_ns = std::chrono::high_resolution_clock::now().time_since_epoch().count();
                }

                if (!execute_operation(operation,
                                       rpc,
                                       sender,
                                       R2_ASYNC_WAIT)) {
                    // Even if an operation fails to execute, mark it finished
                    // so the benchmark can progress.
                }

                if (thread_id == FLAGS_latency_queue_id) {
                    const uint64_t now_ns = std::chrono::high_resolution_clock::now().time_since_epoch().count();
                    const uint64_t lat_ns = now_ns - before_ns;
                    g_ctx.latency_samples.push_back(lat_ns);
                }
                ++local_completed;
                ++state.completed;
            }

            auto& state = g_ctx.thread_states[thread_id];
            if (!g_ctx.running.load(std::memory_order_acquire) &&
                !g_ctx.scheduler_stopped.load(std::memory_order_acquire)) {
                if (!g_ctx.scheduler_stopped.exchange(true, std::memory_order_acq_rel)) {
                    R2_STOP();
                }
            } else if (state.completed >= g_ctx.operations_per_thread && !state.stop_posted) {
                // Per-thread budget consumed: stop this scheduler.
                state.stop_posted = true;
                R2_STOP();
            }

            LOG(2) << "[rolex-target] thread " << thread_id
                   << " coroutine " << R2_COR_ID()
                   << " executed " << local_completed << " operations";

            R2_RET;
        });
    }

    ssched.run();

    auto& state = g_ctx.thread_states[thread_id];
    state.end_time = std::chrono::steady_clock::now();
    const auto finished_threads = g_ctx.threads_completed.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (finished_threads >= g_ctx.thread_count) {
        g_ctx.running.store(false, std::memory_order_release);
    }

    return 0;
}

}  // namespace

int target_on_benchmark_ready(int argc, char **argv) {
    configure_rolex_flags_from_macros();

    for (int i = 0; i < argc; i++)
        std::cout << i << ": " << std::string(argv[i]) << std::endl;

    gflags::ParseCommandLineFlags(&argc, &argv, true);
    set_manual_rank(FLAGS_rank_id, FLAGS_world_size);
    rolex::bench::load_benchmark_config();

    std::string main_file = FLAGS_main_trace_path;
    std::string loop_file = FLAGS_loop_trace_path.empty() ? main_file : FLAGS_loop_trace_path;

    if (main_file.empty()) {
        std::cerr << "main_trace_path flag is required." << std::endl;
        return -1;
    }

    const auto main_override = parse_override_op(FLAGS_main_trace_op);
    if (!main_override && !FLAGS_main_trace_op.empty()) {
        return -1;
    }
    const auto loop_override = parse_override_op(FLAGS_loop_trace_op);
    if (!loop_override && !FLAGS_loop_trace_op.empty()) {
        return -1;
    }

    const auto main_loaded = load_traces(main_file, main_override);
    if (main_loaded == 0) {
        std::cerr << "Main trace file '" << main_file << "' is empty or missing." << std::endl;
        return -1;
    }

    const auto loop_loaded = load_loop_trace(loop_file, loop_override);
    if (loop_loaded == 0) {
        std::cerr << "Loop trace file '" << loop_file << "' is empty or missing." << std::endl;
        return -1;
    }
    std::cout << "Loaded main trace entries: " << main_loaded
              << ", loop trace entries: " << loop_loaded << std::endl;

    g_ctx.thread_count = static_cast<std::size_t>(FLAGS_nthreads);
    g_ctx.coroutine_count = static_cast<std::size_t>(FLAGS_num_coroutines);
    if (g_ctx.thread_count == 0) {
        std::cerr << "nthreads must be greater than zero." << std::endl;
        return -1;
    }
    if (g_ctx.coroutine_count == 0) {
        std::cerr << "num_coroutines must be greater than zero." << std::endl;
        return -1;
    }

    g_ctx.operations_per_thread = FLAGS_ops_per_thread ? FLAGS_ops_per_thread : loop_loaded;
    if (g_ctx.operations_per_thread == 0) {
        std::cerr << "No operations configured per thread." << std::endl;
        return -1;
    }

    g_ctx.running.store(true, std::memory_order_release);
    g_ctx.scheduler_stopped.store(false, std::memory_order_release);
    g_ctx.threads_completed.store(0, std::memory_order_release);
    g_ctx.thread_states.assign(g_ctx.thread_count, ThreadState{});
    for (std::size_t i = 0; i < g_ctx.thread_states.size(); ++i) {
        g_ctx.thread_states[i].op_rank = global_rank() * g_ctx.thread_count + i;
    }
    g_ctx.latency_samples.clear();

    set_operation_rank_span(g_ctx.thread_count * global_world_size());

    return 0;
}

void target_on_benchmark_run() {
    if (g_ctx.thread_count == 0) {
        LOG(1) << "[rolex-target] no operations configured, skipping run";
        return;
    }

    g_ctx.barrier = std::make_unique<xstore::util::PBarrier>(g_ctx.thread_count);
    g_ctx.workers.reserve(g_ctx.thread_count);

    for (std::size_t thread_id = 0; thread_id < g_ctx.thread_count; ++thread_id) {
        g_ctx.workers.emplace_back(
            std::make_unique<r2::Thread<usize>>([thread_id]() -> usize {
                return run_worker(thread_id);
            }));
    }

    for (auto& worker : g_ctx.workers) {
        worker->start();
    }

    while (g_ctx.running.load(std::memory_order_acquire))
        usleep(1000);

    for (auto& worker : g_ctx.workers) {
        worker->join();
    }

    print_latencies();

    uint64_t total_ops = 0;
    std::chrono::steady_clock::time_point earliest_start{};
    std::chrono::steady_clock::time_point latest_end{};
    bool have_times = false;

    for (const auto& state : g_ctx.thread_states) {
        total_ops += state.completed;
        if (state.start_recorded) {
            if (!have_times || state.start_time < earliest_start) {
                earliest_start = state.start_time;
            }
            if (!have_times || state.end_time > latest_end) {
                latest_end = state.end_time;
            }
            have_times = true;
        }
    }

    if (have_times && total_ops > 0) {
        const double duration_sec =
            std::chrono::duration<double>(latest_end - earliest_start).count();
        if (duration_sec > 0.0) {
            const double throughput = static_cast<double>(total_ops) / duration_sec;
            printf("Throughput: %.2f ops/s over %.3f s (%llu operations)\n",
                   throughput,
                   duration_sec,
                   static_cast<unsigned long long>(total_ops));
        } else {
            printf("Throughput: duration too small to measure (%llu operations)\n",
                   static_cast<unsigned long long>(total_ops));
        }
    } else {
        printf("Throughput: no operations recorded.\n");
    }

    g_ctx.running.store(false, std::memory_order_release);
}

void target_thread_init(std::size_t) {}

void target_thread_exit(std::size_t) {}

OperationResult target_do_operation(std::size_t,
                                    std::size_t,
                                    const Operation&) {
    return {};
}

std::vector<std::size_t> target_check_for_replies(std::size_t) {
    return {};
}
