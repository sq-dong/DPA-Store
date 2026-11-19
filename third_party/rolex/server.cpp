
#include <csignal>

#include <benchs/load_config.hh>
#include <benchs/load_data.hh>

#include "rlib/core/nicinfo.hh"
#include "rlib/core/rctrl.hh"
#include "rlib/core/common.hh"

#include "rolex/trait.hpp"
#include "rolex/rolex_server.hpp"

DEFINE_int64(port, 8888, "Server listener (UDP) port.");
DEFINE_uint64(leaf_num, 10000000, "The number of registed leaves.");

thread_local char* rpc_large_reply_buf = nullptr;
thread_local u32 rpc_large_reply_key;
volatile bool init = false;
volatile bool running = false;
std::vector<std::unique_ptr<r2::Thread<usize>>> workers;
std::atomic<size_t> ready_threads(0);
rdmaio::RCtrl* ctrl;
rolex::rolex_t *rolex_index;
rolex::remote_memory_t* RM;
xstore::util::PBarrier* bar;

void signal_handler(int signal)
{
    LOG(2) << "Signal " << signal << " received.";
    running = false;

    for (const auto &worker : workers)
        worker->join();

    LOG(2) << "Stopped threads.";

    delete bar;
}

void rolex_get_callback(const Header& rpc_header, const MemBlock& args, UDTransport* replyc)
{
    // sanity check the requests
    ASSERT(args.sz == sizeof(rolex::KeyType));
    rolex::KeyType key = *(reinterpret_cast<rolex::KeyType*>(args.mem_ptr));

    // GET
    rolex::ValType dummy_value = 1234;
    bool res = rolex_index->search(key, dummy_value);
    rolex::ReplyValue reply;

    if (res) {
        reply = { .status = true, .val = dummy_value };
    } else {
        reply = { .status = false, .val = dummy_value };
    }

    // send
    char reply_buf[64];
    RPCOp op;
    ASSERT(op.set_msg(MemBlock(reply_buf, 64)).set_reply().add_arg(reply));
    op.set_corid(rpc_header.cor_id);
    // LOG(3)<<"GET: " << *(args.interpret_as<u64>());
    ASSERT(op.execute(replyc) == IOCode::Ok);
}

void rolex_put_callback(const Header& rpc_header, const MemBlock& args, UDTransport* replyc)
{
    // sanity check the requests
    ASSERT(args.sz == sizeof(rolex::KeyType) + sizeof(rolex::ValType));
    rolex::KeyType key = *args.interpret_as<rolex::KeyType>();
    rolex::ValType val = *args.interpret_as<rolex::ValType>(sizeof(rolex::KeyType));

    // insert
    rolex_index->insert(key, val);
    rolex::ReplyValue reply;

    // send
    char reply_buf[64];
    RPCOp op;
    ASSERT(op.set_msg(MemBlock(reply_buf, 64)).set_reply().add_arg(reply));
    op.set_corid(rpc_header.cor_id);
    //LOG(3) << "Put key:" << key;
    ASSERT(op.execute(replyc) == IOCode::Ok);
}

void rolex_update_callback(const Header& rpc_header, const MemBlock& args, UDTransport* replyc)
{
    // sanity check the requests
    ASSERT(args.sz == sizeof(rolex::KeyType) + sizeof(rolex::ValType));
    rolex::KeyType key = *args.interpret_as<rolex::KeyType>();
    rolex::ValType val = *args.interpret_as<rolex::ValType>(sizeof(rolex::KeyType));

    // UPDATE
    bool res = rolex_index->update(key, val);
    rolex::ReplyValue reply;
    if (res) {
        reply = { .status = true, .val = val };
    } else {
        reply = { .status = false, .val = val };
    }

    // send
    char reply_buf[64];
    RPCOp op;
    ASSERT(op.set_msg(MemBlock(reply_buf, 64)).set_reply().add_arg(reply));
    op.set_corid(rpc_header.cor_id);
    ASSERT(op.execute(replyc) == IOCode::Ok);
}


void rolex_remove_callback(const Header& rpc_header, const MemBlock& args, UDTransport* replyc)
{
    // sanity check the requests
    ASSERT(args.sz == sizeof(rolex::KeyType));
    rolex::KeyType key = *args.interpret_as<rolex::KeyType>();

    // UPDATE
    bool res = rolex_index->remove(key);
    rolex::ReplyValue reply;
    if(res) {
        reply = { .status = true, .val = 0 };
    } else {
        reply = { .status = false, .val = 0 };
    }

    // send
    char reply_buf[64];
    RPCOp op;
    ASSERT(op.set_msg(MemBlock(reply_buf, 64)).set_reply().add_arg(reply));
    op.set_corid(rpc_header.cor_id);
    ASSERT(op.execute(replyc) == IOCode::Ok);
}


void rolex_scan_callback(const Header& rpc_header, const MemBlock& args, UDTransport* replyc)
{
    // sanity check the requests
    ASSERT(args.sz == sizeof(rolex::KeyType)+sizeof(u64));
    rolex::KeyType key = *args.interpret_as<rolex::KeyType>();
    rolex::ValType n = *args.interpret_as<rolex::ValType>(sizeof(rolex::KeyType));

    // UPDATE
    std::vector<rolex::ValType> result;
    rolex_index->range(key, n, result);
    rolex::ReplyValue reply;

    // send
    char reply_buf[64];
    RPCOp op;
    ASSERT(op.set_msg(MemBlock(reply_buf, 64)).set_reply().add_arg(reply));
    op.set_corid(rpc_header.cor_id);
    ASSERT(op.execute(replyc) == IOCode::Ok);
}

void run()
{
    const usize MB = 1024 * 1024;
    ctrl = new RCtrl(FLAGS_port);
    bar = new xstore::util::PBarrier(rolex::bench::fLU64::FLAGS_threads);

    /* Remote memory setup */
    rolex::RM_config conf(ctrl,
                          1024 * MB,
                          FLAGS_leaf_num * sizeof(rolex::leaf_t),
                          rolex::bench::FLAGS_reg_leaf_region,
                          FLAGS_leaf_num);
    RM = new rolex::remote_memory_t(conf);

    rolex::load_data();

    LOG(2) << "[processing data]";
    std::sort(rolex::exist_keys.begin(), rolex::exist_keys.end());
    rolex::exist_keys.erase(std::unique(rolex::exist_keys.begin(),
                                               rolex::exist_keys.end()),
                            rolex::exist_keys.end());
    std::sort(rolex::exist_keys.begin(), rolex::exist_keys.end());

    for (size_t i = 1; i < rolex::exist_keys.size(); i++)
        assert(rolex::exist_keys[i] >= rolex::exist_keys[i - 1]);

    rolex_index = new rolex::rolex_t(RM, rolex::exist_keys, rolex::exist_keys);
    rolex_index->print_data();

    RDMA_LOG(2) << "Server started!";
    RM->start_daemon();

    running = true;
    RDMA_LOG(2) << "Starting " << rolex::bench::fLU64::FLAGS_threads << " threads.";

    /* Create threads */
    for (size_t thread_id = 0; thread_id < rolex::bench::fLU64::FLAGS_threads; thread_id++) {
        workers.emplace_back(std::make_unique<r2::Thread<usize>>([thread_id, ctrl] () -> usize {
            /* NIC stuff */
            auto nic_for_recv = RNic::create(RNicInfo::query_dev_names().at(0)).value();
            auto ud_qp = UD::create(nic_for_recv, QPConfig()).value();

            /* local MR */
            auto mem_region = HugeRegion::create(64 * 1024 * 1024).value();
            auto mem = mem_region->convert_to_rmem().value();
            auto handler = RegHandler::create(mem, nic_for_recv).value();

            /* Post receive buffers to QP and transition QP to RTR state */
            SimpleAllocator alloc(mem, handler->get_reg_attr().value());
            auto recv_rs_at_recv =
                RecvEntriesFactory<SimpleAllocator, RECV_NUM, 4096>::create(alloc);
            auto res = ud_qp->post_recvs(*recv_rs_at_recv, RECV_NUM);
            RDMA_ASSERT(res == IOCode::Ok);

            /* register the UD for connection */
            ctrl->registered_qps.reg("b" + std::to_string(thread_id), ud_qp);
            LOG(2) << "server thread #" << thread_id << " started!";
            RPCCore<UDTransport, UDRecvTransport<RECV_NUM>, UDSessionManager<RECV_NUM>> rpc(12);

            auto large_buf = alloc.alloc_one(1024).value();
            rpc_large_reply_buf = static_cast<char*>(std::get<0>(large_buf));
            rpc_large_reply_key = std::get<1>(large_buf);

            UDRecvTransport<RECV_NUM> recv(ud_qp, recv_rs_at_recv);

            // register the callbacks before enter the main loop
            ASSERT(rpc.reg_callback(rolex_get_callback) == rolex::GET);
            ASSERT(rpc.reg_callback(rolex_put_callback) == rolex::PUT);
            ASSERT(rpc.reg_callback(rolex_update_callback) == rolex::UPDATE);
            ASSERT(rpc.reg_callback(rolex_remove_callback) == rolex::DELETE);
            ASSERT(rpc.reg_callback(rolex_scan_callback) == rolex::SCAN);
            r2::compile_fence();

            bar->wait();
            while (!init)
                r2::compile_fence();

            while (running) {
                r2::compile_fence();
                rpc.recv_event_loop(&recv);
            }

            return 0;
        }));
    };

    LOG(2) << "Created threads.";

    for (const auto &worker : workers)
        worker->start();

    init = true;
}

void stop(void)
{
    LOG(2) << "Stopping...";

    delete ctrl;
    delete rolex_index;
    delete RM;
}

int main(int argc, char** argv)
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    rolex::bench::load_benchmark_config();
    std::signal(SIGINT, signal_handler);

    run();

    while (running)
        usleep(500000);

    stop();

    return 0;
}
