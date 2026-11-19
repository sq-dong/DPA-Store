
#include <memory>

#include <benchs/load_config.hh>
#include <benchs/load_data.hh>

#include "r2/src/thread.hh"
#include "rlib/core/nicinfo.hh"
#include "rlib/core/rctrl.hh"
#include "rlib/core/common.hh"

#include "rolex/trait.hpp"
#include "xcomm/src/rpc/mod.hh"
#include "xcomm/src/transport/rdma_ud_t.hh"
#include "xcomm/tests/transport_util.hh"
#include "xutils/local_barrier.hh"

DEFINE_int64(port, 8888, "Server listener (UDP) port.");
DEFINE_string(hostname, "10.0.1.12", "Server hostname or IP.");
DEFINE_uint64(leaf_num, 10000000, "The number of registed leaves.");
DEFINE_uint32(nic_index, 0, "Index of the NIC interface to use.");
DEFINE_uint32(nthreads, 1, "Number of threads.");
DEFINE_uint32(num_coroutines, 12, "Number of co-routines.");

using namespace rolex;

volatile bool running = false;
std::vector<std::unique_ptr<r2::Thread<usize>>> workers;
std::atomic<size_t> ready_threads(0);
xstore::util::PBarrier *bar;

using RPC = rpc::RPCCore<transport::UDTransport, transport::UDRecvTransport<2048>, transport::UDSessionManager<2048>>;

void signal_handler(int signal)
{
    LOG(2) << "Signal " << signal << " received.";
    running = false;

    for (const auto &worker : workers)
        worker->join();

    LOG(2) << "Stopped threads.";

    delete bar;
}

auto remote_search(const rolex::KeyType& key, RPC& rpc, transport::UDTransport& sender, R2_ASYNC) -> ::r2::Option<ValType>
{
    char send_buf[64];
    char reply_buf[sizeof(ReplyValue)];

    rpc::RPCOp op;

    op.set_msg(MemBlock(send_buf, 64))
      .set_req()
      .set_rpc_id(GET)
      .set_corid(R2_COR_ID())
      .add_one_reply(rpc.reply_station, {reply_buf, sizeof(ReplyValue)})
      .add_arg<rolex::KeyType>(key);

    ASSERT(rpc.reply_station.cor_ready(R2_COR_ID()) == false);
    auto ret = op.execute_w_key(&sender, 0);
    ASSERT(ret == IOCode::Ok);

    // yield the coroutine to wait for reply
    // LOG(2) << "Waiting for reply";
    R2_PAUSE_AND_YIELD;
    // LOG(2) << "Received reply";

    // check the rest
    ReplyValue r = *(reinterpret_cast<ReplyValue*>(reply_buf));

    if (r.status)
        return (ValType)r.val;

    return {};
}

void remote_put(const rolex::KeyType& key, const ValType& val, RPC& rpc, transport::UDTransport& sender, R2_ASYNC)
{
    std::string data;
    data += ::xstore::util::Marshal<rolex::KeyType>::serialize_to(key);
    data += ::xstore::util::Marshal<ValType>::serialize_to(val);

    char send_buf[64];
    char reply_buf[sizeof(ReplyValue)];

    rpc::RPCOp op;
    op.set_msg(MemBlock(send_buf, 64))
      .set_req()
      .set_rpc_id(PUT)
      .set_corid(R2_COR_ID())
      .add_one_reply(rpc.reply_station, {reply_buf,sizeof(ReplyValue)})
      .add_opaque(data);

    ASSERT(op.execute_w_key(&sender, 0) == IOCode::Ok);

    // yield to the next coroutine
    R2_PAUSE_AND_YIELD;
}

void remote_update(const rolex::KeyType& key, const ValType& val, RPC& rpc, transport::UDTransport& sender, R2_ASYNC)
{
    std::string data;
    data += ::xstore::util::Marshal<rolex::KeyType>::serialize_to(key);
    data += ::xstore::util::Marshal<ValType>::serialize_to(val);

    char send_buf[64];
    char reply_buf[sizeof(ReplyValue)];

    rpc::RPCOp op;
    op.set_msg(MemBlock(send_buf, 64))
      .set_req()
      .set_rpc_id(UPDATE)
      .set_corid(R2_COR_ID())
      .add_one_reply(rpc.reply_station, {reply_buf,sizeof(ReplyValue)})
      .add_opaque(data);

    ASSERT(op.execute_w_key(&sender, 0) == IOCode::Ok);

    // yield to the next coroutine
    R2_PAUSE_AND_YIELD;
}

void remote_remove(const rolex::KeyType& key, RPC& rpc, transport::UDTransport& sender, R2_ASYNC)
{
    std::string data;
    data += ::xstore::util::Marshal<rolex::KeyType>::serialize_to(key);

    char send_buf[64];
    char reply_buf[sizeof(ReplyValue)];

    rpc::RPCOp op;
    op.set_msg(MemBlock(send_buf, 64))
      .set_req()
      .set_rpc_id(DELETE)
      .set_corid(R2_COR_ID())
      .add_one_reply(rpc.reply_station, {reply_buf,sizeof(ReplyValue)})
      .add_opaque(data);

    ASSERT(op.execute_w_key(&sender, 0) == IOCode::Ok);

    // yield to the next coroutine
    R2_PAUSE_AND_YIELD;
}

void remote_scan(const rolex::KeyType& key, const u64& n, RPC& rpc, transport::UDTransport& sender, R2_ASYNC)
{
    std::string data;
    data += ::xstore::util::Marshal<rolex::KeyType>::serialize_to(key);
    data += ::xstore::util::Marshal<u64>::serialize_to(n);

    char send_buf[64];
    char reply_buf[sizeof(ReplyValue)];

    rpc::RPCOp op;
    op.set_msg(MemBlock(send_buf, 64))
      .set_req()
      .set_rpc_id(SCAN)
      .set_corid(R2_COR_ID())
      .add_one_reply(rpc.reply_station, {reply_buf,sizeof(ReplyValue)})
      .add_opaque(data);

    ASSERT(op.execute_w_key(&sender, 0) == IOCode::Ok);

    // yield to the next coroutine
    R2_PAUSE_AND_YIELD;
}

void run(void)
{
    bar = new xstore::util::PBarrier(FLAGS_nthreads);
    std::string server_addr = FLAGS_hostname + ":" + std::to_string(FLAGS_port);

    load_data();
    LOG(2) << "[processing data]";
    std::sort(exist_keys.begin(), exist_keys.end());
    exist_keys.erase(std::unique(exist_keys.begin(), exist_keys.end()), exist_keys.end());
    std::sort(exist_keys.begin(), exist_keys.end());

    for (size_t i = 1; i < exist_keys.size(); i++)
        assert(exist_keys[i] >= exist_keys[i - 1]);

    running = true;

    /* Create threads */
    for (size_t thread_id = 0; thread_id < FLAGS_nthreads; thread_id++) {
        workers.emplace_back(std::make_unique<r2::Thread<usize>>([thread_id, server_addr]() -> usize {
            /* NIC stuff */
            auto nic_for_sender = RNic::create(RNicInfo::query_dev_names().at(FLAGS_nic_index)).value();
            auto ud_qp = UD::create(nic_for_sender, QPConfig()).value();

            /* local MR */
            auto mem_region = HugeRegion::create(16 * 1024 * 1024).value();
            auto mem = mem_region->convert_to_rmem().value();
            auto handler = RegHandler::create(mem, nic_for_sender).value();

            test::SimpleAllocator alloc(mem, handler->get_reg_attr().value());
            auto recv_rs_at_send = RecvEntriesFactory<test::SimpleAllocator, 2048, 1024>::create(alloc);

            RDMA_ASSERT(ud_qp->post_recvs(*recv_rs_at_send, 2048) == IOCode::Ok);

            transport::UDTransport sender;

            r2::Timer t;
            do {
                std::string param = "b" + std::to_string((int)thread_id);
                if (sender.connect(server_addr, param,
                                   thread_id, ud_qp) == IOCode::Ok) {
                    LOG(2) << "Thread " << thread_id << " connect to remote server";
                    break;
                } else {
                    LOG(2) << "Failed to conenct to server " << server_addr << " with param " << param;
                }

                if (t.passed_sec() >= 10)
                    ASSERT(false) << "connection failed at thread #" << thread_id;
            } while (t.passed_sec() < 10);

            /* Prepare send/recv buffers */
            RPC rpc(FLAGS_num_coroutines);
            auto send_buf = std::get<0>(alloc.alloc_one(4096).value());
            ASSERT(send_buf != nullptr);
            auto lkey = handler->get_reg_attr().value().key;
            memset(send_buf, 0, 4096);

            // first we send the connect transport
            auto conn_op = rpc::RPCOp::get_connect_op(MemBlock(send_buf, 2048),
                                                             sender.get_connect_data().value());
            ASSERT(conn_op.execute_w_key(&sender, lkey) == IOCode::Ok);
            rpc::UDRecvTransport<2048> recv_s(ud_qp, recv_rs_at_send);

            // used for other schemes
            size_t non_exist_key_n_per_thread = nonexist_keys.size() / FLAGS_nthreads;
            size_t non_exist_key_start = thread_id * non_exist_key_n_per_thread;
            size_t non_exist_key_end = (thread_id + 1) * non_exist_key_n_per_thread;
            std::vector<u64> op_keys(nonexist_keys.begin() + non_exist_key_start,
                                     nonexist_keys.begin() + non_exist_key_end);
            size_t query_i = 0, insert_i = 0, remove_i = 0, update_i = 0;

            RDMA_ASSERT(exist_keys.size()) << "Bad exist array size";
            RDMA_ASSERT(nonexist_keys.size()) << "Bad nonexist array size";

            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_real_distribution<> ratio_dis(0, 1);

            SScheduler ssched;
            rpc.reg_poll_future(ssched, &recv_s);
            bar->wait();

            if(bench::BenConfig.workloads >= NORMAL) {
                for (size_t i = 0; i < BenConfig.coros; i++) {
                    ssched.spawn([i, send_buf, &rpc, &sender, &recv_s, lkey,
                                 thread_id, &ratio_dis, &gen,
                                 &query_i, &insert_i, &remove_i, &update_i] (R2_ASYNC) {
                        LOG(2) << "Starting thread #" << thread_id << ", co-routine #" << i;
                        char reply_buf[1024];
                        rpc::RPCOp op;
                        uint64_t num_ops = 0;

                        while (running) {
                            // LOG(2) << thread_id << "|" << i << ": Executed " << num_ops << ".";
                            double d = ratio_dis(gen);
                            if (d <= BenConfig.read_ratio) {
                                rolex::KeyType dummy_key = exist_keys[query_i % exist_keys.size()];
                                auto res = remote_search(dummy_key, rpc, sender, R2_ASYNC_WAIT);
                                query_i++;
                                if (unlikely(query_i == exist_keys.size()))
                                    query_i = 0;

                            } else if (d <= BenConfig.read_ratio + BenConfig.insert_ratio) {
                                rolex::KeyType dummy_key = nonexist_keys[insert_i % nonexist_keys.size()];
                                remote_put(dummy_key, dummy_key, rpc, sender, R2_ASYNC_WAIT);
                                insert_i++;
                                if (unlikely(insert_i == nonexist_keys.size()))
                                    insert_i = 0;

                            } else if (d <= BenConfig.read_ratio + BenConfig.insert_ratio + BenConfig.update_ratio) {
                                rolex::KeyType dummy_key = exist_keys[update_i % exist_keys.size()];
                                remote_update(dummy_key, dummy_key, rpc, sender, R2_ASYNC_WAIT);
                                update_i++;
                                if (unlikely(update_i == exist_keys.size()))
                                    update_i = 0;

                            } else {
                                rolex::KeyType dummy_key = exist_keys[remove_i % exist_keys.size()];
                                remote_remove(dummy_key, rpc, sender, R2_ASYNC_WAIT);
                                remove_i++;
                                if (unlikely(remove_i == exist_keys.size()))
                                    remove_i = 0;
                            }

                            num_ops++;
                        }

                        LOG(2) << thread_id << "|" << i << ": Executed " << num_ops << ".";
                        if (R2_COR_ID() == BenConfig.coros)
                            R2_STOP();

                        R2_RET;
                    });
                }
            }
            ssched.run();

            return 0;
        }));
    }

    LOG(2) << "Created threads.";

    for (const auto &worker : workers)
        worker->start();
}

int main(int argc, char** argv)
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    rolex::bench::load_benchmark_config();

    LOG(2) << "Started ROLEX client.";

    run();

    while (running);

    return 0;
}
