
#include "config_shared.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <exception>
#include <limits>
#include <mutex>
#include <random>
#include <infiniband/verbs.h>
#include <stdio.h>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include <vector>

#include <wrapper_flexio.hpp>
#include <common/dpa_common.h>
#include <common/dpastore_common.h>
#include <common/workload_config.hpp>
#include <common/partition_layout.hpp>
#include <common/zipf_sampling.hpp>
#include <common/uniform_catalog.h>

extern "C"
{
    flexio_func_t dpastore_device_init_dpastore_reader;
    flexio_func_t dpastore_device_event_handler_dpastore_reader;
    flexio_func_t print_and_reset_periodic_throughput; // RPC to print and reset periodic throughput
    flexio_func_t dpastore_device_apply_throughput_config;
    flexio_func_t dpastore_device_set_trace_windows;
    extern struct flexio_app *dpa_client_dev;
}

// All site-specific values (DPU MAC, server NIC MAC, FlexIO device name)
// come from common/dpastore_common_config.h via dpastore_common.h.

class dpastore_config_worker_thread {
public:
    FLEX::CQ *rq_cq;
    FLEX::CQ *sq_cq;
    FLEX::SQ *sq;
    FLEX::RQ *rq;

    FLEX::dr_flow_rule *rx_flow_rule;

    FLEX::dr_flow_rule *tx_flow_root_rule;
    FLEX::dr_flow_rule *tx_flow_rule;
};

class dpastore_config {
public:
    std::string device_name;
    FLEX::Context *ctx;

    dpastore_config_worker_thread dpastore_configs_reader[DPASTORE_READER_THREAD_COUNT];

    FLEX::dr_flow_table* global_rx_flow_table = nullptr;
    FLEX::dr_flow_table* global_tx_flow_root_table = nullptr;
    FLEX::dr_flow_table* global_tx_flow_table = nullptr;
    FLEX::DR* global_tx_dr = nullptr;

    bool rq_buffer_on_host = false;
};

DOCA_LOG_REGISTER(DPA_STORE);

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

struct ThroughputCliOptions {
    bool alpha_override = false;
    double alpha = 0.0;
    bool seed_override = false;
    uint64_t seed = 0ULL;
    bool dump_sample = false;
    bool insert_ratio_override = false;
    double insert_ratio = 0.0;
    throughput_key_mode read_mode = THROUGHPUT_KEY_MODE_ZIPF;
    bool read_mode_override = false;
    throughput_key_mode insert_mode = THROUGHPUT_KEY_MODE_ZIPF;
    bool insert_mode_override = false;
    bool warmup_override = false;
    double warmup_seconds = 0.0;
    bool range_mode = false;
    std::string main_trace_path;
    bool main_trace_path_provided = false;
    std::string main_trace_op;
    bool main_trace_op_provided = false;
    std::string loop_trace_path;
    bool loop_trace_path_provided = false;
    std::string loop_trace_op;
    bool loop_trace_op_provided = false;
    uint16_t main_one_of = 1;
    bool wrap_main = true;
};

static ThroughputCliOptions parse_cli(int argc, char **argv)
{
    ThroughputCliOptions opts;
    auto parse_mode = [](std::string_view value, throughput_key_mode &out) {
        std::string token(value);
        std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (token == "random") {
            out = THROUGHPUT_KEY_MODE_RANDOM;
            return true;
        }
        if (token == "uniform") {
            out = THROUGHPUT_KEY_MODE_UNIFORM;
            return true;
        }
        if (token == "zipf") {
            out = THROUGHPUT_KEY_MODE_ZIPF;
            return true;
        }
        return false;
    };

    constexpr std::string_view ZIPF_ALPHA_PREFIX = "--zipf-alpha=";
    constexpr std::string_view ZIPF_SEED_PREFIX = "--zipf-seed=";
    constexpr std::string_view INSERT_RATIO_PREFIX = "--insert-ratio=";
    constexpr std::string_view READ_DIST_PREFIX = "--read-dist=";
    constexpr std::string_view INSERT_DIST_PREFIX = "--insert-dist=";
    constexpr std::string_view WARMUP_SECONDS_PREFIX = "--warmup-seconds=";
    constexpr std::string_view RANGE_FLAG = "--range";
    constexpr std::string_view MAIN_TRACE_PATH_PREFIX = "--main-trace-path=";
    constexpr std::string_view MAIN_TRACE_OP_PREFIX = "--main-trace-op=";
    constexpr std::string_view LOOP_TRACE_PATH_PREFIX = "--loop-trace-path=";
    constexpr std::string_view LOOP_TRACE_OP_PREFIX = "--loop-trace-op=";
    constexpr std::string_view MAIN_ONE_OF_PREFIX = "--main-one-of=";
    constexpr std::string_view WRAP_MAIN_FLAG = "--trace-wrap-main";
    constexpr std::string_view NO_WRAP_MAIN_FLAG = "--trace-no-wrap-main";

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg.rfind(ZIPF_ALPHA_PREFIX, 0) == 0) {
            try {
                std::string_view param = arg.substr(ZIPF_ALPHA_PREFIX.size());
                double value = std::stod(std::string(param));
                if (value <= 1.0) {
                    value = 1.01;
                    DOCA_LOG_WARN("zipf-alpha must be greater than 1.0; clamping to %.2f", value);
                }
                opts.alpha_override = true;
                opts.alpha = value;
            } catch (const std::exception &e) {
                DOCA_LOG_WARN("Failed to parse %.*s (%s); keeping default Zipf alpha",
                              static_cast<int>(arg.size()), arg.data(), e.what());
            }
            continue;
        }

        if (arg.rfind(ZIPF_SEED_PREFIX, 0) == 0) {
            try {
                std::string_view param = arg.substr(ZIPF_SEED_PREFIX.size());
                opts.seed = std::stoull(std::string(param));
                opts.seed_override = true;
            } catch (const std::exception &e) {
                DOCA_LOG_WARN("Failed to parse %.*s (%s); keeping default Zipf seed",
                              static_cast<int>(arg.size()), arg.data(), e.what());
            }
            continue;
        }

        if (arg == "--zipf-dump-sample") {
            opts.dump_sample = true;
            continue;
        }

        if (arg.rfind(INSERT_RATIO_PREFIX, 0) == 0) {
            try {
                std::string_view param = arg.substr(INSERT_RATIO_PREFIX.size());
                double value = std::stod(std::string(param));
                if (value < 0.0) {
                    DOCA_LOG_WARN("insert-ratio must be >= 0.0; clamping to 0.0");
                    value = 0.0;
                } else if (value > 1.0) {
                    DOCA_LOG_WARN("insert-ratio must be <= 1.0; clamping to 1.0");
                    value = 1.0;
                }
                opts.insert_ratio_override = true;
                opts.insert_ratio = value;
            } catch (const std::exception &e) {
                DOCA_LOG_WARN("Failed to parse %.*s (%s); keeping default insert-ratio",
                              static_cast<int>(arg.size()), arg.data(), e.what());
            }
            continue;
        }

        if (arg.rfind(READ_DIST_PREFIX, 0) == 0) {
            std::string_view value = arg.substr(READ_DIST_PREFIX.size());
            throughput_key_mode mode;
            if (parse_mode(value, mode)) {
                opts.read_mode = mode;
                opts.read_mode_override = true;
            } else {
                DOCA_LOG_WARN("Unknown read-dist '%.*s'; keeping default",
                              static_cast<int>(value.size()), value.data());
            }
            continue;
        }

        if (arg.rfind(INSERT_DIST_PREFIX, 0) == 0) {
            std::string_view value = arg.substr(INSERT_DIST_PREFIX.size());
            throughput_key_mode mode;
            if (parse_mode(value, mode)) {
                opts.insert_mode = mode;
                opts.insert_mode_override = true;
            } else {
                DOCA_LOG_WARN("Unknown insert-dist '%.*s'; keeping default",
                              static_cast<int>(value.size()), value.data());
            }
            continue;
        }

        if (arg.rfind(WARMUP_SECONDS_PREFIX, 0) == 0) {
            try {
                std::string_view param = arg.substr(WARMUP_SECONDS_PREFIX.size());
                double value = std::stod(std::string(param));
                if (value < 0.0) {
                    DOCA_LOG_WARN("warmup-seconds must be >= 0.0; clamping to 0.0");
                    value = 0.0;
                }
                opts.warmup_override = true;
                opts.warmup_seconds = value;
            } catch (const std::exception &e) {
                DOCA_LOG_WARN("Failed to parse %.*s (%s); keeping default warmup-seconds",
                              static_cast<int>(arg.size()), arg.data(), e.what());
            }
            continue;
        }

        if (arg == RANGE_FLAG) {
            opts.range_mode = true;
            continue;
        }

        if (arg.rfind(MAIN_TRACE_PATH_PREFIX, 0) == 0) {
            opts.main_trace_path = std::string(arg.substr(MAIN_TRACE_PATH_PREFIX.size()));
            opts.main_trace_path_provided = true;
            continue;
        }

        if (arg.rfind(MAIN_TRACE_OP_PREFIX, 0) == 0) {
            std::string token(arg.substr(MAIN_TRACE_OP_PREFIX.size()));
            std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (token == "get") {
                opts.main_trace_op = "get";
                opts.main_trace_op_provided = true;
            } else if (token == "insert") {
                opts.main_trace_op = "insert";
                opts.main_trace_op_provided = true;
            } else if (token == "range") {
                opts.main_trace_op = "range";
                opts.main_trace_op_provided = true;
            } else {
                DOCA_LOG_WARN("Unknown main-trace-op '%s'; ignoring", token.c_str());
            }
            continue;
        }

        if (arg.rfind(LOOP_TRACE_PATH_PREFIX, 0) == 0) {
            opts.loop_trace_path = std::string(arg.substr(LOOP_TRACE_PATH_PREFIX.size()));
            opts.loop_trace_path_provided = true;
            continue;
        }

        if (arg.rfind(LOOP_TRACE_OP_PREFIX, 0) == 0) {
            std::string token(arg.substr(LOOP_TRACE_OP_PREFIX.size()));
            std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (token == "get") {
                opts.loop_trace_op = "get";
                opts.loop_trace_op_provided = true;
            } else if (token == "insert") {
                opts.loop_trace_op = "insert";
                opts.loop_trace_op_provided = true;
            } else if (token == "range") {
                opts.loop_trace_op = "range";
                opts.loop_trace_op_provided = true;
            } else {
                DOCA_LOG_WARN("Unknown loop-trace-op '%s'; ignoring", token.c_str());
            }
            continue;
        }

        if (arg.rfind(MAIN_ONE_OF_PREFIX, 0) == 0) {
            try {
                std::string_view param = arg.substr(MAIN_ONE_OF_PREFIX.size());
                uint64_t val = std::stoull(std::string(param));
                if (val == 0ULL || val > UINT16_MAX) {
                    DOCA_LOG_WARN("main-one-of must be between 1 and %u; clamping", UINT16_MAX);
                    val = std::max<uint64_t>(1, std::min<uint64_t>(val, UINT16_MAX));
                }
                opts.main_one_of = static_cast<uint16_t>(val);
            } catch (const std::exception &e) {
                DOCA_LOG_WARN("Failed to parse %.*s (%s); keeping default main-one-of=1",
                              static_cast<int>(arg.size()), arg.data(), e.what());
            }
            continue;
        }

        if (arg == WRAP_MAIN_FLAG) {
            opts.wrap_main = true;
            continue;
        }
        if (arg == NO_WRAP_MAIN_FLAG) {
            opts.wrap_main = false;
            continue;
        }
    }
    return opts;
}

struct TraceResources {
    std::unique_ptr<uint64_t, void(*)(void*)> buffer{nullptr, free};
    size_t key_count = 0;
    size_t padded_key_count = 0;
    size_t keys_per_line = 0;
    size_t line_count = 0;
    size_t bytes = 0;
    ibv_mr *mr = nullptr;
};

static uint8_t trace_op_code_from_string(const std::string &op)
{
    if (op == "get") {
        return DPASTORE_OP_GET;
    }
    if (op == "insert") {
        return DPASTORE_OP_INSERT;
    }
    if (op == "range") {
        return DPASTORE_OP_RANGE;
    }
    return 0xFFu;
}

static bool load_trace_keys(const std::string &path,
                            size_t keys_per_line,
                            TraceResources &out)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        DOCA_LOG_ERR("Failed to open trace file %s", path.c_str());
        return false;
    }

    std::vector<uint64_t> keys;
    std::string line;
    size_t line_no = 0;
    const size_t max_preview = 8;
    std::vector<uint64_t> preview;
    while (std::getline(input, line)) {
        line_no++;
        if (line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        std::string op;
        uint64_t key = 0;
        if (!(iss >> op >> key)) {
            DOCA_LOG_WARN("Skipping malformed line %zu in %s", line_no, path.c_str());
            continue;
        }
        keys.push_back(key);
        if (preview.size() < max_preview) {
            preview.push_back(key);
        }
    }

    if (keys.empty()) {
        DOCA_LOG_WARN("Trace %s contained no usable keys", path.c_str());
        return false;
    }

    const size_t padded = ((keys.size() + keys_per_line - 1) / keys_per_line) * keys_per_line;
    const size_t bytes = padded * sizeof(uint64_t);

    void *aligned = nullptr;
    if (posix_memalign(&aligned, 64, bytes) != 0 || aligned == nullptr) {
        DOCA_LOG_ERR("posix_memalign failed for trace buffer (%zu bytes)", bytes);
        return false;
    }

    auto deleter = [](void *ptr) { free(ptr); };
    std::unique_ptr<uint64_t, decltype(deleter)> buffer(static_cast<uint64_t *>(aligned), deleter);

    std::memcpy(buffer.get(), keys.data(), keys.size() * sizeof(uint64_t));
    if (padded > keys.size()) {
        const uint64_t pad_value = keys.back();
        for (size_t i = keys.size(); i < padded; ++i) {
            buffer.get()[i] = pad_value;
        }
    }

    out.buffer = std::move(buffer);
    out.key_count = keys.size();
    out.padded_key_count = padded;
    out.keys_per_line = keys_per_line;
    out.line_count = padded / keys_per_line;
    out.bytes = bytes;
    out.mr = nullptr;

    DOCA_LOG_INFO("Loaded trace %s: %zu keys (%zu padded), %zu lines, %zu keys/line",
                  path.c_str(), out.key_count, out.padded_key_count, out.line_count, out.keys_per_line);
    if (!preview.empty()) {
        std::string sample;
        for (uint64_t k : preview) {
            sample += std::to_string(k);
            sample.push_back(' ');
        }
        DOCA_LOG_INFO("Trace preview (first %zu keys): %s", preview.size(), sample.c_str());
    }
    if (out.padded_key_count > 0) {
        const uint64_t first = out.buffer.get()[0];
        const uint64_t last = out.buffer.get()[out.padded_key_count - 1];
        DOCA_LOG_INFO("Trace buffer ends: first=%llu last=%llu (padded_count=%zu)",
                      (unsigned long long)first,
                      (unsigned long long)last,
                      out.padded_key_count);
        size_t stride_idx = keys_per_line * DPASTORE_READER_THREAD_COUNT;
        if (stride_idx + 3 < out.padded_key_count) {
            DOCA_LOG_INFO("Trace stride sample (line %zu): [%llu, %llu, %llu, %llu]",
                          stride_idx / keys_per_line,
                          (unsigned long long)out.buffer.get()[stride_idx + 0],
                          (unsigned long long)out.buffer.get()[stride_idx + 1],
                          (unsigned long long)out.buffer.get()[stride_idx + 2],
                          (unsigned long long)out.buffer.get()[stride_idx + 3]);
        }
    }

    return true;
}

static void dump_sample_keys(const dpastore::ZipfWorkloadConfig &cfg,
                             uint64_t partition_span,
                             uint64_t partition_target,
                             const std::vector<uint32_t> &zipf_table)
{
    const uint32_t thread_id = 0;
    const uint64_t base_key = dpastore::partition_base_key(cfg, thread_id, partition_span);
    const uint64_t limit_key = dpastore::partition_limit_key(cfg, thread_id, partition_span);
    const uint64_t span = limit_key - base_key;

    uint64_t count = partition_target;
    if (count > span) {
        count = span;
    }
    if (count == 0ULL) {
        DOCA_LOG_INFO("Zipf sample: partition span is zero; nothing to dump");
        return;
    }

    UniformPlan plan = dpastore::make_uniform_plan(base_key, span, count,
                                                   dpastore::mix_partition_seed(cfg.seed, thread_id));

    uint64_t rng_state = dpastore::mix_partition_seed(cfg.seed, thread_id);
    const uint32_t table_entries = static_cast<uint32_t>(zipf_table.size());

    constexpr uint32_t sample_count = 128;
    std::vector<uint64_t> keys;
    keys.reserve(sample_count);

    for (uint32_t i = 0; i < sample_count; ++i) {
        uint64_t rnd = dpastore::xorshift64(rng_state);
        uint64_t rank = 0;
        if (table_entries > 0U) {
            uint32_t index = static_cast<uint32_t>((rnd >> 48) % table_entries);
            rank = zipf_table[index];
        } else {
            rank = rnd % count;
        }
        if (rank >= count) {
            rank = rnd % count;
        }
        uint64_t key = uniform_key_for_rank(&plan, rank);
        keys.push_back(key);
    }

    std::string line;
    line.reserve(sample_count * 22);
    for (uint64_t key : keys) {
        line += std::to_string(key);
        line.push_back(' ');
    }
    DOCA_LOG_INFO("Zipf sample (thread 0, first %u draws): %s",
                  sample_count, line.c_str());

    std::vector<uint64_t> unique = keys;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    DOCA_LOG_INFO("Zipf sample stats: unique=%zu span=%llu count=%llu base=%llu limit=%llu",
                  unique.size(),
                  static_cast<unsigned long long>(span),
                  static_cast<unsigned long long>(count),
                  static_cast<unsigned long long>(base_key),
                  static_cast<unsigned long long>(limit_key));
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
    config->ctx->create_event_handler(*event_handler_func, 0);

    wt->rq_cq = new FLEX::CQ(true, CLIENT_RING_DEPTH, config->ctx, current_thread);
    wt->sq_cq = new FLEX::CQ(false, CLIENT_RING_DEPTH, config->ctx, current_thread);
    wt->rq = new FLEX::RQ(CLIENT_RING_DEPTH, CLIENT_ENTRY_BSIZE, wt->rq_cq->get_cq_num(), config->ctx, config->rq_buffer_on_host);
    wt->sq = new FLEX::SQ(CLIENT_RING_DEPTH, CLIENT_ENTRY_BSIZE, wt->sq_cq->get_cq_num(), config->ctx, wt->rq);
    queue_config_data dev_data {
        wt->rq_cq->get_cq_transf(),
        wt->rq->get_rq_transf(),
        wt->sq_cq->get_cq_transf(),
        wt->sq->get_sq_transf(),
        thread_idx, 0, config->ctx->get_window_id()
    };
    flexio_uintptr_t dev_config_data = config->ctx->move_to_dev(dev_data);
    uint64_t rpc_ret_val;
    Assert(flexio_process_call(config->ctx->get_process(), device_init_func, &rpc_ret_val, dev_config_data) ==
        FLEXIO_STATUS_SUCCESS);

    matcher.clear();
    wt->rx_flow_rule = new FLEX::dr_flow_rule();
    wt->rx_flow_rule->add_dest_devx_tir(wt->rq->get_inner_ptr());
    matcher.set_dst_mac(CLIENT_DPU_MAC_HEX);
    matcher.set_dst_udp_port((uint16_t)(udp_port_base + thread_idx));
    wt->rx_flow_rule->create_dr_rule(config->global_rx_flow_table, &matcher);

    matcher.clear();
    wt->tx_flow_rule = new FLEX::dr_flow_rule();
    wt->tx_flow_root_rule = new FLEX::dr_flow_rule();
    wt->tx_flow_root_rule->add_dest_table(config->global_tx_flow_table->dr_table);
    wt->tx_flow_rule->add_dest_vport(config->global_tx_dr->get_inner_ptr(), 0xFFFF);

    matcher.set_dst_mac(SERVER_MAC_HEX);
    wt->tx_flow_root_rule->create_dr_rule(config->global_tx_flow_root_table, &matcher);
    wt->tx_flow_rule->create_dr_rule(config->global_tx_flow_table, &matcher);

    config->ctx->event_handler_run(thread_idx, current_thread);
}

int main(int argc, char **argv) {
    SET_LOG_LEVEL(DOCA_LOG_LEVEL_INFO);

    dpastore_config config;
    config.device_name = CLIENT_DEVICE_NAME;

    config.ctx = new FLEX::Context(config.device_name);
    Assert(config.ctx);

    flexio_version_set(FLEXIO_CURRENT_VERSION);

    // useless at DOCA2.2.1
    // config.ctx->alloc_devx_uar(0x1);
    config.ctx->alloc_pd();

    config.ctx->create_process(dpa_client_dev);

    config.ctx->create_window();

    config.ctx->generate_flexio_uar();
    config.ctx->print_init("dpastore", 0);

    dpastore::ZipfWorkloadConfig workload_cfg = dpastore::load_zipf_config_from_defines();
    auto cli_opts = parse_cli(argc, argv);
    constexpr size_t TRACE_KEYS_PER_LINE = 64 / sizeof(uint64_t);
    TraceResources main_trace;
    TraceResources loop_trace;
    uint8_t main_trace_op_code = 0xFFu;
    uint8_t loop_trace_op_code = 0xFFu;
    bool main_trace_ready = false;
    bool loop_trace_ready = false;
    std::unique_ptr<uint8_t, void(*)(void*)> combined_trace_buffer{nullptr, free};
    ibv_mr *combined_trace_mr = nullptr;
    size_t main_offset_bytes = 0;
    size_t loop_offset_bytes = 0;

    if (cli_opts.main_trace_op_provided) {
        main_trace_op_code = trace_op_code_from_string(cli_opts.main_trace_op);
        if (main_trace_op_code == 0xFFu) {
            DOCA_LOG_WARN("Unsupported main-trace-op '%s'; disabling main trace", cli_opts.main_trace_op.c_str());
            cli_opts.main_trace_path_provided = false;
        }
    } else if (cli_opts.main_trace_path_provided) {
        DOCA_LOG_WARN("main-trace-path provided without main-trace-op; disabling main trace");
        cli_opts.main_trace_path_provided = false;
    }

    if (cli_opts.loop_trace_op_provided) {
        loop_trace_op_code = trace_op_code_from_string(cli_opts.loop_trace_op);
        if (loop_trace_op_code == 0xFFu) {
            DOCA_LOG_WARN("Unsupported loop-trace-op '%s'; disabling loop trace", cli_opts.loop_trace_op.c_str());
            cli_opts.loop_trace_path_provided = false;
        }
    } else if (cli_opts.loop_trace_path_provided) {
        DOCA_LOG_WARN("loop-trace-path provided without loop-trace-op; disabling loop trace");
        cli_opts.loop_trace_path_provided = false;
    }

    if (!cli_opts.main_trace_path_provided && cli_opts.main_trace_op_provided) {
        DOCA_LOG_WARN("main-trace-op specified without main-trace-path; ignoring main trace");
    }
    if (!cli_opts.loop_trace_path_provided && cli_opts.loop_trace_op_provided) {
        DOCA_LOG_WARN("loop-trace-op specified without loop-trace-path; ignoring loop trace");
    }

    if (cli_opts.alpha_override) {
        workload_cfg.alpha = cli_opts.alpha;
    }
    if (cli_opts.seed_override && cli_opts.seed != 0ULL) {
        workload_cfg.seed = cli_opts.seed;
    }

    /* Dual-trace setup (main + loop) */
    auto load_and_map_trace = [&](const std::string &path,
                                  TraceResources &res,
                                  const char *label,
                                  bool &ready) {
        ready = false;
        if (!load_trace_keys(path, TRACE_KEYS_PER_LINE, res)) {
            DOCA_LOG_WARN("%s trace load failed; skipping", label);
            return;
        }
        ready = true;
    };

    if (cli_opts.main_trace_path_provided && cli_opts.main_trace_op_provided) {
        load_and_map_trace(cli_opts.main_trace_path, main_trace, "main", main_trace_ready);
    }
    if (cli_opts.loop_trace_path_provided && cli_opts.loop_trace_op_provided) {
        load_and_map_trace(cli_opts.loop_trace_path, loop_trace, "loop", loop_trace_ready);
    }

    bool trace_mode = main_trace_ready || loop_trace_ready;
    if (trace_mode) {
        size_t main_offset = 0;
        size_t loop_offset = 0;
        size_t combined_bytes = 0;
        if (main_trace_ready) {
            main_offset = combined_bytes;
            combined_bytes += main_trace.bytes;
        }
        if (loop_trace_ready) {
            loop_offset = combined_bytes;
            combined_bytes += loop_trace.bytes;
        }

        if (combined_bytes == 0) {
            main_trace_ready = false;
            loop_trace_ready = false;
        } else {
            void *combined_ptr = nullptr;
            if (posix_memalign(&combined_ptr, 64, combined_bytes) != 0 || combined_ptr == nullptr) {
                DOCA_LOG_ERR("Failed to allocate combined trace buffer (%zu bytes)", combined_bytes);
                main_trace_ready = false;
                loop_trace_ready = false;
            } else {
                auto deleter = [](void *p) { free(p); };
                std::unique_ptr<uint8_t, decltype(deleter)> combined(static_cast<uint8_t *>(combined_ptr), deleter);

                if (main_trace_ready) {
                    std::memcpy(static_cast<uint8_t *>(combined_ptr) + main_offset,
                                main_trace.buffer.get(),
                                main_trace.bytes);
                }
                if (loop_trace_ready) {
                    std::memcpy(static_cast<uint8_t *>(combined_ptr) + loop_offset,
                                loop_trace.buffer.get(),
                                loop_trace.bytes);
                }

                ibv_mr *combined_mr = config.ctx->reg_mr(combined.get(), combined_bytes,
                                                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
                if (combined_mr == nullptr) {
                    DOCA_LOG_ERR("Failed to register combined trace buffer (%zu bytes)", combined_bytes);
                    main_trace_ready = false;
                    loop_trace_ready = false;
                } else {
                    struct {
                        flexio_uintptr_t base_host_addr;
                        uint32_t window_id;
                        uint32_t mkey;
                        uint64_t main_offset_bytes;
                        uint64_t loop_offset_bytes;
                    } trace_window_info {
                        .base_host_addr = (flexio_uintptr_t)combined.get(),
                        .window_id = config.ctx->get_window_id(),
                        .mkey = combined_mr->lkey,
                        .main_offset_bytes = main_offset,
                        .loop_offset_bytes = loop_offset
                    };
                    flexio_uintptr_t trace_window_devaddr = config.ctx->move_to_dev(trace_window_info);
                    uint64_t rpc_ret_val = 0;
                    auto rpc_status = flexio_process_call(config.ctx->get_process(),
                                                          &dpastore_device_set_trace_windows,
                                                          &rpc_ret_val,
                                                          trace_window_devaddr);
                    if (rpc_status == FLEXIO_STATUS_SUCCESS && rpc_ret_val == 0ULL) {
                        DOCA_LOG_INFO("Combined trace window mapped to device (main_ready=%d loop_ready=%d)",
                                      main_trace_ready ? 1 : 0, loop_trace_ready ? 1 : 0);
                        combined_trace_buffer.reset(combined.release());
                        combined_trace_mr = combined_mr;
                        main_trace.bytes = combined_bytes;
                        main_offset_bytes = main_offset;
                        loop_offset_bytes = loop_offset;
                        main_trace.buffer.reset();
                        loop_trace.buffer.reset();
                    } else {
                        DOCA_LOG_WARN("Failed to map combined trace window (status=%d, ret=%llu); disabling trace mode",
                                      (int)rpc_status, (unsigned long long)rpc_ret_val);
                        main_trace_ready = false;
                        loop_trace_ready = false;
                        ibv_dereg_mr(combined_mr);
                    }
                }
            }
        }
    }
    trace_mode = main_trace_ready || loop_trace_ready;

    const uint64_t partition_span = dpastore::compute_partition_span(workload_cfg);
    const uint64_t partition_target = dpastore::compute_partition_target(workload_cfg);
    if (workload_cfg.table_size == 0 || workload_cfg.table_size > THROUGHPUT_ZIPF_TABLE_MAX_ENTRIES) {
        workload_cfg.table_size = static_cast<uint32_t>(std::min<uint64_t>(partition_target,
            static_cast<uint64_t>(THROUGHPUT_ZIPF_TABLE_MAX_ENTRIES)));
        if (workload_cfg.table_size == 0) {
            workload_cfg.table_size = 1;
        }
    }

    auto zipf_vector = dpastore::make_zipf_table(workload_cfg, partition_target, workload_cfg.table_size);
    std::array<uint32_t, THROUGHPUT_ZIPF_TABLE_MAX_ENTRIES> zipf_table{};
    std::copy(zipf_vector.begin(), zipf_vector.end(), zipf_table.begin());

    if (cli_opts.dump_sample) {
        dump_sample_keys(workload_cfg, partition_span, partition_target, zipf_vector);
    }

    double insert_ratio = std::clamp(cli_opts.insert_ratio, 0.0, 1.0);
    if (trace_mode) {
        insert_ratio = 0.0;
    }
    const double read_ratio = trace_mode ? 1.0 : std::max(0.0, 1.0 - insert_ratio);

    const auto ratio_to_q16 = [](double ratio) -> uint32_t {
        double clamped = std::clamp(ratio, 0.0, 1.0);
        return static_cast<uint32_t>(std::lround(clamped * 65535.0));
    };
    uint16_t insert_ratio_q16 = static_cast<uint16_t>(ratio_to_q16(insert_ratio));

    auto describe_mode = [](throughput_key_mode mode) -> const char * {
        switch (mode) {
            case THROUGHPUT_KEY_MODE_RANDOM:
                return "random";
            case THROUGHPUT_KEY_MODE_UNIFORM:
                return "uniform";
            case THROUGHPUT_KEY_MODE_ZIPF:
            default:
                return "zipf";
        }
    };

    const bool range_mode_flag = cli_opts.range_mode ||
        (trace_mode && ((main_trace_ready && main_trace_op_code == DPASTORE_OP_RANGE) ||
                        (loop_trace_ready && loop_trace_op_code == DPASTORE_OP_RANGE)));

    if (trace_mode) {
        DOCA_LOG_INFO("Trace mode enabled: main[%s op=%s ready=%d] loop[%s op=%s ready=%d] stride=%u main-one-of=%u wrap-main=%d",
                      cli_opts.main_trace_path.c_str(),
                      cli_opts.main_trace_op.c_str(),
                      main_trace_ready ? 1 : 0,
                      cli_opts.loop_trace_path.c_str(),
                      cli_opts.loop_trace_op.c_str(),
                      loop_trace_ready ? 1 : 0,
                      DPASTORE_READER_THREAD_COUNT,
                      cli_opts.main_one_of,
                      cli_opts.wrap_main ? 1 : 0);
    } else {
        DOCA_LOG_INFO("Throughput client op mix: read=%.2f%% (%s), insert=%.2f%% (%s)%s",
                      read_ratio * 100.0,
                      describe_mode(cli_opts.read_mode),
                      insert_ratio * 100.0,
                      describe_mode(cli_opts.insert_mode),
                      range_mode_flag ? " [range responses enabled]" : "");
    }

    const auto make_runtime_cfg = [&](uint16_t insert_ratio_q16_param) {
        throughput_zipf_config cfg{};
        cfg.zipf_table_entries = workload_cfg.table_size;
        cfg.partition_span = partition_span;
        cfg.partition_count = partition_target;
        cfg.key_min = workload_cfg.min_key;
        cfg.key_max = workload_cfg.max_key;
        cfg.seed = workload_cfg.seed;
        cfg.alpha_q16 = dpastore::encode_alpha_q16(workload_cfg.alpha);
        cfg.enable_key_partitioning = 1;
        cfg.insert_ratio_q16 = insert_ratio_q16_param;
        cfg.read_distribution = static_cast<uint8_t>(cli_opts.read_mode);
        cfg.insert_distribution = static_cast<uint8_t>(cli_opts.insert_mode);
        cfg.total_keys = workload_cfg.total_keys;
        cfg.range_mode = range_mode_flag ? 1 : 0;
        cfg.main_trace_enabled = main_trace_ready ? 1 : 0;
        cfg.loop_trace_enabled = loop_trace_ready ? 1 : 0;
        cfg.main_trace_op = main_trace_ready ? main_trace_op_code : 0;
        cfg.loop_trace_op = loop_trace_ready ? loop_trace_op_code : 0;
        cfg.wrap_main = cli_opts.wrap_main ? 1 : 0;
        cfg.main_one_of = cli_opts.main_one_of;
        cfg.main_trace_keys_per_line = main_trace_ready ? static_cast<uint32_t>(main_trace.keys_per_line) : 0;
        cfg.loop_trace_keys_per_line = loop_trace_ready ? static_cast<uint32_t>(loop_trace.keys_per_line) : 0;
        cfg.main_trace_total_keys = main_trace_ready ? main_trace.key_count : 0;
        cfg.loop_trace_total_keys = loop_trace_ready ? loop_trace.key_count : 0;
        cfg.main_trace_line_count = main_trace_ready ? main_trace.line_count : 0;
        cfg.loop_trace_line_count = loop_trace_ready ? loop_trace.line_count : 0;
        cfg.main_trace_offset_bytes = main_trace_ready ? main_offset_bytes : 0;
        cfg.loop_trace_offset_bytes = loop_trace_ready ? loop_offset_bytes : 0;
        return cfg;
    };

    throughput_zipf_config runtime_cfg_active = make_runtime_cfg(insert_ratio_q16);

    throughput_zipf_config runtime_cfg_warmup = make_runtime_cfg(0);

    flexio_uintptr_t zipf_table_devaddr = config.ctx->move_to_dev(zipf_table);
    runtime_cfg_active.zipf_table_devaddr = zipf_table_devaddr;
    runtime_cfg_warmup.zipf_table_devaddr = zipf_table_devaddr;

    const bool warmup_enabled = (!trace_mode) && cli_opts.warmup_override && cli_opts.warmup_seconds > 0.0;
    const double warmup_seconds = warmup_enabled ? cli_opts.warmup_seconds : 0.0;

    throughput_zipf_config initial_cfg = warmup_enabled ? runtime_cfg_warmup : runtime_cfg_active;
    flexio_uintptr_t runtime_cfg_dev = config.ctx->move_to_dev(initial_cfg);

    uint64_t rpc_ret_val = 0; // For RPC return values
    Assert(flexio_process_call(config.ctx->get_process(), &dpastore_device_apply_throughput_config,
                               &rpc_ret_val, runtime_cfg_dev) == FLEXIO_STATUS_SUCCESS);

    std::atomic<bool> warmup_switch_done{!warmup_enabled};
    std::thread warmup_thread;
    std::mutex warmup_mutex;
    std::condition_variable warmup_cv;
    bool warmup_cancel = false;
    if (warmup_enabled) {
        DOCA_LOG_INFO("Warmup enabled: %.2f seconds of GET-only traffic before enabling inserts",
                      warmup_seconds);
        FLEX::Context *warmup_ctx = config.ctx;
        warmup_thread = std::thread([warmup_seconds,
                                     warmup_ctx,
                                     runtime_cfg_active,
                                     &warmup_switch_done,
                                     &warmup_mutex,
                                     &warmup_cv,
                                     &warmup_cancel]() {
            std::unique_lock<std::mutex> lock(warmup_mutex);
            bool cancelled = warmup_cv.wait_for(lock, std::chrono::duration<double>(warmup_seconds),
                                                [&] { return warmup_cancel; });
            lock.unlock();

            if (cancelled || warmup_switch_done.exchange(true)) {
                return;
            }

            throughput_zipf_config cfg = runtime_cfg_active;
            flexio_uintptr_t cfg_dev = warmup_ctx->move_to_dev(cfg);
            uint64_t rpc_ret = 0;
            if (flexio_process_call(warmup_ctx->get_process(), &dpastore_device_apply_throughput_config,
                                     &rpc_ret, cfg_dev) == FLEXIO_STATUS_SUCCESS) {
                DOCA_LOG_INFO("Warmup complete: enabling configured insert ratios");
            } else {
                DOCA_LOG_WARN("Warmup reconfiguration failed; staying in GET-only mode");
            }
        });
    }

    if (!trace_mode) {
        DOCA_LOG_INFO("Throughput client: workload %s, span=%llu, target/thread=%llu, table=%u",
                      dpastore::describe(workload_cfg).c_str(),
                      static_cast<unsigned long long>(partition_span),
                      static_cast<unsigned long long>(partition_target),
                      runtime_cfg_active.zipf_table_entries);
    } else {
        DOCA_LOG_INFO("Throughput client: trace replay active main_ready=%d loop_ready=%d (main_keys=%zu loop_keys=%zu)",
                      main_trace_ready ? 1 : 0,
                      loop_trace_ready ? 1 : 0,
                      main_trace_ready ? main_trace.key_count : 0,
                      loop_trace_ready ? loop_trace.key_count : 0);
    }

    FLEX::flow_matcher matcher{};

    auto *global_rx_dr = new FLEX::DR(config.ctx, MLX5DV_DR_DOMAIN_TYPE_NIC_RX);
    matcher.set_dst_mac_mask();
    matcher.set_dst_udp_port_mask();
    config.global_rx_flow_table = global_rx_dr->create_flow_table(0, 0, &matcher);

    matcher.clear();

    config.global_tx_dr = new FLEX::DR(config.ctx, MLX5DV_DR_DOMAIN_TYPE_FDB);
    matcher.set_dst_mac_mask();
    config.global_tx_flow_root_table = config.global_tx_dr->create_flow_table(0, 0, &matcher);
    config.global_tx_flow_table = config.global_tx_dr->create_flow_table(1, 0, &matcher);

    matcher.clear();

    uint32_t current_thread = 0;

    // Reader Threads
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


    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    DOCA_LOG_INFO("DPA Throughput Client Started");
    DOCA_LOG_INFO("Press Ctrl+C to terminate");

    // Add periodic throughput measurement every 2 seconds
    auto last_throughput_print = std::chrono::steady_clock::now();

    while (!force_quit) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_throughput_print);

        if (elapsed.count() >= 2) {
            // Call device RPC to print and reset periodic throughput stats
            if (flexio_process_call(config.ctx->get_process(), &print_and_reset_periodic_throughput, &rpc_ret_val, 0) == FLEXIO_STATUS_SUCCESS) {
                // Stats printed by device
            }
            last_throughput_print = now;
        }

        sleep(1);
    }

    if (warmup_thread.joinable()) {
        {
            std::lock_guard<std::mutex> lock(warmup_mutex);
            warmup_cancel = true;
        }
        warmup_cv.notify_all();
        warmup_switch_done.store(true);
        warmup_thread.join();
    }

    // Print throughput statistics on shutdown
    DOCA_LOG_INFO("=== DPA Throughput Client Shutdown Statistics ===");

    // Call device-side printing function for final stats
    if (flexio_process_call(config.ctx->get_process(), &print_and_reset_periodic_throughput, &rpc_ret_val, 0) == FLEXIO_STATUS_SUCCESS) {
        DOCA_LOG_INFO("Device statistics printed above");
    } else {
        DOCA_LOG_WARN("Failed to call device stats printing function");
    }

    DOCA_LOG_INFO("=== Shutdown Complete ===");

    return EXIT_SUCCESS;
}
