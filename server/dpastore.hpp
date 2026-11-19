#include "wrapper_flexio.hpp"
#include "common/dpa_common.h"

extern "C"
{
    flexio_func_t dpastore_device_init_dpastore_reader;
    flexio_func_t dpastore_device_init_dpastore_updater;
    flexio_func_t dpastore_device_event_handler_dpastore_reader;
    flexio_func_t dpastore_device_event_handler_dpastore_updater;
    flexio_func_t dpastore_device_set_global_area; // RPC to set the global area pointer
    flexio_func_t dpastore_device_set_host_global_area; // RPC to set the host global area window info
    flexio_func_t dpastore_device_stitcher_thread; // Event handler for stitcher thread
    flexio_func_t dpastore_device_init_stitcher; // RPC to initialize stitcher thread
    flexio_func_t get_total_periodic_count; // RPC to get total periodic count
    flexio_func_t get_per_thread_periodic_counts; // RPC to get per-thread periodic counts
    flexio_func_t print_and_reset_periodic_throughput; // RPC to print and reset periodic throughput
    extern struct flexio_app *dpastore_dev;
}

// FlexIO device name comes from common/dpastore_common_config.h
// (SERVER_DEVICE_NAME).  Edit there to override.

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
    dpastore_config_worker_thread dpastore_configs_updater[DPASTORE_UPDATER_THREAD_COUNT];

    FLEX::dr_flow_table* global_rx_flow_table = nullptr;
    FLEX::dr_flow_table* global_tx_flow_root_table = nullptr;
    FLEX::dr_flow_table* global_tx_flow_table = nullptr;
    FLEX::DR* global_tx_dr = nullptr;

    bool rq_buffer_on_host = false;
};
