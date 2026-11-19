#include <arpa/inet.h>
#include <signal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cctype>
#include <cstdint>
#include <csignal>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gflags/gflags.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_memory.h>
#include <rte_thash.h>
#include <rte_udp.h>

#include "backend.hpp"
#include "common.h"
#include "config.hpp"
#include "protocol.hpp"
#include "target_interface.hpp"

#include "dpastore_common_config.h"

#define TARGET_STR ("[" TARGET_NAME "] ")

DEFINE_string(trace_file, "", "Absolute path to trace file consumed by all threads.");
DEFINE_string(dev_name, "mlx5_0", "DPDK device name (rte_eth_dev_get_port_by_name).");
DEFINE_uint64(ops_per_thread, 8096, "Number of operations to be done per thread (0 = infinite).");
DEFINE_uint32(src_port, DPASTORE_UDPPORT_CLIENT, "Base client source port.");
DEFINE_uint32(dst_port_base, DPASTORE_UDPPORT_READER, "Base port of server.");
DEFINE_uint32(num_server_readers, DPASTORE_READER_THREAD_COUNT,
              "Number of server DPA reader/traverser threads (must match server build).");
// CONFIGURE FOR YOUR SITE: lab default is the dbar-b (10.0.1.12) NIC; override
// at runtime with -dst_ip=<server IP>.
DEFINE_string(dst_ip, "10.0.1.12", "Server IP.");
DEFINE_string(src_ip, "", "Client IP (use interface default if empty).");
DEFINE_string(src_mac, "", "Client MAC address (XX:XX:XX:XX:XX:XX) to use; default is NIC MAC.");
DEFINE_string(dst_mac, "", "Server MAC address (XX:XX:XX:XX:XX:XX).");
DEFINE_string(main_trace_path, "", "Main trace file path (no wrap unless --trace_wrap_main). Defaults to trace_file.");
DEFINE_string(loop_trace_path, "", "Loop trace file path (wrap). Defaults to main trace.");
DEFINE_string(main_trace_op, "", "Override operation for main trace (GET/READ/INSERT/UPDATE/DELETE/RANGE/SCAN/PUT/PING).");
DEFINE_string(loop_trace_op, "", "Override operation for loop trace (GET/READ/INSERT/UPDATE/DELETE/RANGE/SCAN/PUT/PING).");
DEFINE_uint32(main_one_of, 20, "Issue one main-trace op every N operations. 0 disables main trace.");
DEFINE_bool(trace_wrap_main, false, "Wrap main trace when exhausted.");
DEFINE_bool(trace_no_wrap_main, false, "Disable main trace wrapping (alias).");
DEFINE_uint32(rank_id, 0, "Global rank of this process (pass from launcher).");
DEFINE_uint32(world_size, 1, "Global world size (pass from launcher).");
DEFINE_string(barrier_file, "", "If set, wait for this file to exist before starting traffic.");
DEFINE_uint32(num_inflight, 64, "Outstanding operations per shard thread.");
DEFINE_uint32(num_shard_threads, 0, "Number of shard threads (each does TX+RX). Defaults to num_tx_threads if 0.");
DEFINE_uint32(num_tx_threads, 15, "Compatibility flag: used when num_shard_threads is 0.");
DEFINE_uint32(num_rx_threads, 0, "Compatibility flag; ignored.");
DEFINE_uint32(num_rb_threads, 0, "Compatibility flag; ignored.");
DEFINE_uint32(starvation_time, 100,
              "Time a packet is considered lost in transport, in µs. A slot is reclaimed after this.");
DEFINE_uint32(latency_queue_id, 0, "Queue index to sample latencies from.");

namespace {

constexpr uint16_t kTxDesc = 1024;
constexpr uint16_t kRxDesc = 1024;
constexpr uint16_t kMbufPoolCache = 512;
constexpr uint32_t kMbufsPerQueue = 8192;
constexpr uint16_t kRxBurst = 32;

#ifndef RTE_THASH_V4_L4_LEN
#define RTE_THASH_V4_L4_LEN 3
#endif
#ifndef RTE_ETH_RETA_GROUP_SIZE
#define RTE_ETH_RETA_GROUP_SIZE 64
#endif
#ifndef RTE_ETH_RSS_RETA_SIZE_128
#define RTE_ETH_RSS_RETA_SIZE_128 128
#endif
#ifndef RTE_ETH_RSS_RETA_SIZE_512
#define RTE_ETH_RSS_RETA_SIZE_512 RTE_ETH_RSS_RETA_SIZE_128
#endif
#ifndef RTE_ETH_RSS_KEY_LEN
#define RTE_ETH_RSS_KEY_LEN 52
#endif
#ifndef RTE_ETH_RSS_NONFRAG_IPV4_UDP
#define RTE_ETH_RSS_NONFRAG_IPV4_UDP RTE_ETH_RSS_UDP
#endif

struct SoftrssV4Tuple {
    uint32_t src_addr;
    uint32_t dst_addr;
    uint32_t ports;  // high 16: src_port, low 16: dst_port (host order)
};

struct ShardCtx {
    uint16_t queue_id{0};
    uint16_t lcore_id{0};
    uint16_t socket_id{0};
    uint32_t inflight{0};
    std::size_t op_rank{0};
    uint64_t loop_idx{0};
    uint64_t main_idx{0};
    bool main_exhausted{false};
    uint32_t main_stride_counter{0};
    uint64_t sent{0};
    uint64_t received{0};
    uint64_t timed_out{0};
    uint64_t rss_mismatch{0};
    std::vector<uint64_t> slot_ts_cycles;
    std::unordered_map<uint16_t, uint16_t> src_port_cache;
    std::vector<uint64_t> latency_samples;  // Only for latency_queue_id.
};

struct RssState {
    std::vector<uint8_t> key;
    uint32_t key_len{0};
    std::vector<uint16_t> reta;
};

struct {
    uint16_t port_id{0};
    uint32_t num_shards{0};
    uint32_t num_inflight{0};
    uint64_t timer_hz{0};
    uint64_t timeout_cycles{0};
    std::atomic<bool> stop{false};

    uint32_t src_ip_be{0};
    uint32_t dst_ip_be{0};
    uint32_t src_ip_host{0};
    uint32_t dst_ip_host{0};
    rte_ether_addr src_mac{};
    rte_ether_addr dst_mac{};

    RssState rss;
    std::vector<rte_mempool *> mempools;
    std::vector<ShardCtx> shards;
    std::vector<uint16_t> shard_lcores;
} ctx;

inline uint64_t cycles_now() {
    return rte_get_timer_cycles();
}

inline uint64_t cycles_to_ns(uint64_t cycles) {
    return (cycles * 1000000000ull) / ctx.timer_hz;
}

void log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, TARGET_STR);
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::printf(TARGET_STR);
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
}

void signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        ctx.stop.store(true, std::memory_order_relaxed);
    }
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

    log_error("Unknown operation override '%s' (expected GET/READ/INSERT/UPDATE/PUT/DELETE/RANGE/SCAN/PING)",
              op_str.c_str());
    return std::nullopt;
}

int load_trace_file_or_fail() {
    std::string main_file = FLAGS_main_trace_path.empty() ? FLAGS_trace_file : FLAGS_main_trace_path;
    std::string loop_file = FLAGS_loop_trace_path.empty() ? main_file : FLAGS_loop_trace_path;

    if (main_file.empty()) {
        log_error("main_trace_path/trace_file flag is required");
        return -EINVAL;
    }

    const auto main_override = parse_override_op(FLAGS_main_trace_op);
    const auto loop_override = parse_override_op(FLAGS_loop_trace_op);

    if (!main_override && !FLAGS_main_trace_op.empty()) {
        return -EINVAL;
    }
    if (!loop_override && !FLAGS_loop_trace_op.empty()) {
        return -EINVAL;
    }

    const auto main_loaded = load_traces(main_file, main_override);
    if (main_loaded == 0) {
        log_error("Main trace file '%s' is empty or missing", main_file.c_str());
        return -EINVAL;
    }

    const auto loop_loaded = load_loop_trace(loop_file, loop_override);
    if (loop_loaded == 0) {
        log_error("Loop trace file '%s' is empty or missing", loop_file.c_str());
        return -EINVAL;
    }

    return 0;
}

int resolve_port_id(uint16_t &port_id) {
    int ret = rte_eth_dev_get_port_by_name(FLAGS_dev_name.c_str(), &port_id);
    if (ret != 0) {
        uint16_t pid;
        RTE_ETH_FOREACH_DEV(pid) {
            port_id = pid;
            log_info("Falling back to first available port %u after failing to find %s", port_id,
                     FLAGS_dev_name.c_str());
            return 0;
        }
        log_error("Failed to find port by name %s: %s", FLAGS_dev_name.c_str(), rte_strerror(-ret));
        return ret;
    }
    return 0;
}

bool parse_mac(const std::string &mac_str, rte_ether_addr *out) {
    unsigned int bytes[6];
    if (std::sscanf(mac_str.c_str(), "%x:%x:%x:%x:%x:%x", &bytes[0], &bytes[1], &bytes[2], &bytes[3],
                    &bytes[4], &bytes[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        out->addr_bytes[i] = static_cast<std::uint8_t>(bytes[i]);
    }
    return true;
}

int configure_addresses() {
    in_addr addr{};
    if (!inet_aton(FLAGS_dst_ip.c_str(), &addr)) {
        log_error("Failed to parse dst_ip flag");
        return -EINVAL;
    }
    ctx.dst_ip_be = addr.s_addr;
    ctx.dst_ip_host = rte_be_to_cpu_32(ctx.dst_ip_be);

    if (!FLAGS_src_ip.empty()) {
        if (!inet_aton(FLAGS_src_ip.c_str(), &addr)) {
            log_error("Failed to parse src_ip flag");
            return -EINVAL;
        }
        ctx.src_ip_be = addr.s_addr;
    } else {
        // CONFIGURE FOR YOUR SITE: lab default src IP (dbar-a, 10.0.1.11);
        // override at runtime with -src_ip=<your client IP>.
        inet_aton("10.0.1.11", &addr);
        ctx.src_ip_be = addr.s_addr;
    }
    ctx.src_ip_host = rte_be_to_cpu_32(ctx.src_ip_be);
    return 0;
}

void fill_ether(struct rte_ether_hdr *eth, const rte_ether_addr *src_mac, const rte_ether_addr *dst_mac) {
    rte_ether_addr_copy(dst_mac, &eth->dst_addr);
    rte_ether_addr_copy(src_mac, &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
}

void fill_ipv4(struct rte_ipv4_hdr *ipv4, rte_be32_t src_ip, rte_be32_t dst_ip, uint16_t udp_payload_len) {
    ipv4->version_ihl = (4u << 4) | (sizeof(struct rte_ipv4_hdr) >> 2);
    ipv4->type_of_service = 0;
    ipv4->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + udp_payload_len);
    ipv4->packet_id = 0;
    ipv4->fragment_offset = 0;
    ipv4->time_to_live = 64;
    ipv4->next_proto_id = IPPROTO_UDP;
    ipv4->src_addr = src_ip;
    ipv4->dst_addr = dst_ip;
    ipv4->hdr_checksum = 0;
}

void fill_udp(struct rte_udp_hdr *udp, uint16_t src_port, uint16_t dst_port, uint16_t udp_payload_len) {
    udp->src_port = rte_cpu_to_be_16(src_port);
    udp->dst_port = rte_cpu_to_be_16(dst_port);
    udp->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + udp_payload_len);
    udp->dgram_cksum = 0;
}

protocol::KVPacket *fill_packet(struct rte_mbuf *pkt, uint16_t src_port, uint16_t dst_port) {
    auto *eth = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
    auto *ipv4 = reinterpret_cast<struct rte_ipv4_hdr *>(eth + 1);
    auto *udp = reinterpret_cast<struct rte_udp_hdr *>(ipv4 + 1);
    auto *kv = reinterpret_cast<protocol::KVPacket *>(udp + 1);

    fill_ether(eth, &ctx.src_mac, &ctx.dst_mac);
    fill_ipv4(ipv4, ctx.src_ip_be, ctx.dst_ip_be, sizeof(protocol::KVPacket));
    fill_udp(udp, src_port, dst_port, sizeof(protocol::KVPacket));

    const uint16_t len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) +
                         sizeof(protocol::KVPacket);
    pkt->data_len = len;
    pkt->pkt_len = len;
    pkt->l2_len = sizeof(struct rte_ether_hdr);
    pkt->l3_len = sizeof(struct rte_ipv4_hdr);
    pkt->l4_len = sizeof(struct rte_udp_hdr);
    pkt->ol_flags = RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_UDP_CKSUM;

    return kv;
}

uint16_t rss_queue_for_reply(uint16_t server_port, uint16_t candidate_src_port) {
    if (ctx.rss.reta.empty() || ctx.rss.key.empty()) {
        return 0;
    }

    SoftrssV4Tuple tuple{
        ctx.dst_ip_host,
        ctx.src_ip_host,
        static_cast<uint32_t>(server_port) << 16 | static_cast<uint32_t>(candidate_src_port),
    };

    const uint32_t hash = rte_softrss(reinterpret_cast<uint32_t *>(&tuple), RTE_THASH_V4_L4_LEN, ctx.rss.key.data());
    const uint32_t idx = hash % ctx.rss.reta.size();
    return ctx.rss.reta[idx];
}

uint16_t pick_src_port(ShardCtx &shard, uint16_t dst_port) {
    auto it = shard.src_port_cache.find(dst_port);
    if (it != shard.src_port_cache.end()) {
        return it->second;
    }

    const uint16_t start = static_cast<uint16_t>(FLAGS_src_port + shard.queue_id);
    uint16_t best = start;
    for (uint32_t step = 0; step < 65536u; ++step) {
        const uint16_t cand = static_cast<uint16_t>(start + step);
        if (cand < 1024) {
            continue;
        }
        if (rss_queue_for_reply(dst_port, cand) == shard.queue_id) {
            best = cand;
            break;
        }
    }
    shard.src_port_cache.emplace(dst_port, best);
    return best;
}

int update_reta(uint16_t port_id, uint16_t num_rxq) {
    rte_eth_dev_info info{};
    int ret = rte_eth_dev_info_get(port_id, &info);
    if (ret != 0) {
        log_error("Failed to query device info: %s", rte_strerror(-ret));
        return ret;
    }

    uint16_t reta_size = info.reta_size ? info.reta_size : RTE_ETH_RSS_RETA_SIZE_128;
    ctx.rss.reta.assign(reta_size, 0);
    for (uint16_t i = 0; i < reta_size; ++i) {
        ctx.rss.reta[i] = static_cast<uint16_t>(i % num_rxq);
    }

    std::array<rte_eth_rss_reta_entry64, RTE_ETH_RSS_RETA_SIZE_512 / RTE_ETH_RETA_GROUP_SIZE> reta_conf{};
    for (uint16_t i = 0; i < reta_size; ++i) {
        auto &entry = reta_conf[i / RTE_ETH_RETA_GROUP_SIZE];
        entry.mask = UINT64_MAX;
        entry.reta[i % RTE_ETH_RETA_GROUP_SIZE] = ctx.rss.reta[i];
    }

    ret = rte_eth_dev_rss_reta_update(port_id, reta_conf.data(), reta_size);
    if (ret != 0) {
        log_error("Failed to program RETA: %s", rte_strerror(-ret));
        return ret;
    }
    return 0;
}

int fetch_rss_key(uint16_t port_id) {
    if (ctx.rss.key.empty()) {
        return 0;
    }
    rte_eth_dev_info info{};
    int ret = rte_eth_dev_info_get(port_id, &info);
    if (ret != 0) {
        log_error("Failed to query device info for RSS key: %s", rte_strerror(-ret));
        return ret;
    }

    ctx.rss.key_len = info.hash_key_size ? info.hash_key_size : RTE_ETH_RSS_KEY_LEN;
    ctx.rss.key.assign(ctx.rss.key_len, 0);

    rte_eth_rss_conf rss_conf{};
    rss_conf.rss_key = ctx.rss.key.data();
    rss_conf.rss_key_len = ctx.rss.key_len;
    rss_conf.rss_hf = RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_NONFRAG_IPV4_UDP;

    ret = rte_eth_dev_rss_hash_conf_get(port_id, &rss_conf);
    if (ret != 0) {
        log_info("rss_hash_conf_get failed (%s), keeping configured RSS key", rte_strerror(-ret));
        return 0;
    }
    ctx.rss.key_len = rss_conf.rss_key_len;
    ctx.rss.key.assign(rss_conf.rss_key, rss_conf.rss_key + rss_conf.rss_key_len);
    return 0;
}

int create_mempools(uint16_t num_queues, const std::vector<uint16_t> &queue_sockets) {
    ctx.mempools.resize(num_queues, nullptr);
    for (uint16_t q = 0; q < num_queues; ++q) {
        char name[32];
        std::snprintf(name, sizeof(name), "mbuf_q%u", q);
        const uint16_t socket = queue_sockets[q];
        ctx.mempools[q] = rte_pktmbuf_pool_create(name, kMbufsPerQueue, kMbufPoolCache, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                                  socket);
        if (!ctx.mempools[q]) {
            log_error("Cannot create mbuf pool for queue %u on socket %u: %s", q, socket,
                      rte_strerror(rte_errno));
            return -rte_errno;
        }
    }
    return 0;
}

int setup_device(uint16_t port_id, uint16_t num_queues, const std::vector<uint16_t> &queue_sockets) {
    rte_eth_dev_info dev_info{};
    int ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret != 0) {
        log_error("Failed to get device info: %s", rte_strerror(-ret));
        return ret;
    }
    if (num_queues > dev_info.max_rx_queues || num_queues > dev_info.max_tx_queues) {
        log_error("Device supports %u RX / %u TX queues, requested %u", dev_info.max_rx_queues,
                  dev_info.max_tx_queues, num_queues);
        return -EINVAL;
    }

    if (ctx.rss.key.empty()) {
        const uint32_t key_len = dev_info.hash_key_size ? dev_info.hash_key_size : RTE_ETH_RSS_KEY_LEN;
        ctx.rss.key.resize(key_len);
        for (uint32_t i = 0; i < key_len; ++i) {
            ctx.rss.key[i] = static_cast<uint8_t>((i * 0x5a) + 1);
        }
        ctx.rss.key_len = key_len;
    }

    rte_eth_conf port_conf{};
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    port_conf.rx_adv_conf.rss_conf.rss_key = ctx.rss.key.data();
    port_conf.rx_adv_conf.rss_conf.rss_key_len = ctx.rss.key.size();
    port_conf.rx_adv_conf.rss_conf.rss_hf = RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_NONFRAG_IPV4_UDP;
    port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;
    port_conf.rxmode.offloads = 0;
    port_conf.txmode.offloads = dev_info.tx_offload_capa &
                                (RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                                 RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE);

    ret = rte_eth_dev_configure(port_id, num_queues, num_queues, &port_conf);
    if (ret != 0) {
        log_error("Failed to configure device: %s", rte_strerror(-ret));
        return ret;
    }

    uint16_t rx_desc = kRxDesc;
    uint16_t tx_desc = kTxDesc;
    rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &rx_desc, &tx_desc);

    rte_eth_rxconf rx_conf = dev_info.default_rxconf;
    rx_conf.offloads = port_conf.rxmode.offloads;
    rte_eth_txconf tx_conf = dev_info.default_txconf;
    tx_conf.offloads = port_conf.txmode.offloads;

    for (uint16_t q = 0; q < num_queues; ++q) {
        ret = rte_eth_rx_queue_setup(port_id, q, rx_desc, queue_sockets[q], &rx_conf, ctx.mempools[q]);
        if (ret != 0) {
            log_error("Failed to setup RX queue %u: %s", q, rte_strerror(-ret));
            return ret;
        }
        ret = rte_eth_tx_queue_setup(port_id, q, tx_desc, queue_sockets[q], &tx_conf);
        if (ret != 0) {
            log_error("Failed to setup TX queue %u: %s", q, rte_strerror(-ret));
            return ret;
        }
    }

    if ((ret = update_reta(port_id, num_queues)) != 0) {
        return ret;
    }
    (void)fetch_rss_key(port_id);

    ret = rte_eth_dev_start(port_id);
    if (ret != 0) {
        log_error("Failed to start device: %s", rte_strerror(-ret));
        return ret;
    }
    rte_eth_promiscuous_enable(port_id);
    return 0;
}

bool all_shards_finished() {
    for (const auto &s : ctx.shards) {
        if (FLAGS_ops_per_thread && s.sent < FLAGS_ops_per_thread) {
            return false;
        }
        if (s.inflight) {
            return false;
        }
    }
    return true;
}

int find_slot(ShardCtx &shard, uint64_t now_cycles) {
    for (uint32_t i = 0; i < ctx.num_inflight; ++i) {
        uint64_t ts = shard.slot_ts_cycles[i];
        if (ts == 0) {
            return static_cast<int>(i);
        }
        if (now_cycles - ts > ctx.timeout_cycles) {
            shard.slot_ts_cycles[i] = 0;
            shard.timed_out++;
            shard.inflight--;
            return static_cast<int>(i);
        }
    }
    return -1;
}

void reap_timeouts(ShardCtx &shard, uint64_t now_cycles) {
    for (uint32_t i = 0; i < ctx.num_inflight; ++i) {
        uint64_t ts = shard.slot_ts_cycles[i];
        if (ts && now_cycles - ts > ctx.timeout_cycles) {
            shard.slot_ts_cycles[i] = 0;
            shard.timed_out++;
            if (shard.inflight > 0) {
                shard.inflight--;
            }
        }
    }
}

int shard_loop(void *arg) {
    auto *shard = reinterpret_cast<ShardCtx *>(arg);
    rte_mbuf *rx_burst[kRxBurst];

    while (!ctx.stop.load(std::memory_order_relaxed)) {
        const uint64_t now_cycles = cycles_now();
        reap_timeouts(*shard, now_cycles);

        // Receive path
        const uint16_t rx = rte_eth_rx_burst(ctx.port_id, shard->queue_id, rx_burst, kRxBurst);
        for (uint16_t i = 0; i < rx; ++i) {
            rte_mbuf *pkt = rx_burst[i];
            auto *eth = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
            auto *ipv4 = reinterpret_cast<struct rte_ipv4_hdr *>(eth + 1);
            auto *udp = reinterpret_cast<struct rte_udp_hdr *>(ipv4 + 1);
            auto *kv = reinterpret_cast<protocol::KVPacket *>(udp + 1);

            (void)eth;
            (void)ipv4;

            if (kv->queue_id != shard->queue_id) {
                shard->rss_mismatch++;
                const uint16_t sport = rte_be_to_cpu_16(udp->src_port);
                const uint16_t dport = rte_be_to_cpu_16(udp->dst_port);
                // if (shard->rss_mismatch <= 8 || (shard->rss_mismatch & ((1 << 20) - 1)) == 0) {
                //     log_error("RSS mismatch on shard %u: kv queue_id %u (sport %u dport %u, slot %u)",
                //               shard->queue_id, kv->queue_id, sport, dport, kv->slot_id);
                // }
            }

            const uint16_t slot_id = kv->slot_id;
            if (slot_id < shard->slot_ts_cycles.size() && shard->slot_ts_cycles[slot_id]) {
                shard->slot_ts_cycles[slot_id] = 0;
                shard->inflight--;
            }

            if (shard->queue_id == FLAGS_latency_queue_id) {
                const uint64_t now_ns = cycles_to_ns(now_cycles);
                const uint64_t lat_ns = now_ns - kv->timestamp;
                shard->latency_samples.push_back(lat_ns);
            }
            shard->received++;
            rte_pktmbuf_free(pkt);
        }

        if (FLAGS_ops_per_thread && shard->sent >= FLAGS_ops_per_thread && shard->inflight == 0) {
            break;
        }

        // Transmit path
        const bool unlimited_inflight = ctx.num_inflight == 0;
        while ((!FLAGS_ops_per_thread || shard->sent < FLAGS_ops_per_thread) &&
               (unlimited_inflight || shard->inflight < ctx.num_inflight)) {
            if (ctx.stop.load(std::memory_order_relaxed)) {
                break;
            }
            bool use_loop = true;
            if (FLAGS_main_one_of > 0) {
                if (shard->main_stride_counter + 1 >= FLAGS_main_one_of) {
                    use_loop = false;
                    shard->main_stride_counter = 0;
                } else {
                    shard->main_stride_counter++;
                }
            }
            std::optional<Operation> op_opt;
            if (use_loop) {
                op_opt = get_loop_operation(shard->loop_idx, shard->op_rank);
                if (!op_opt) {
                    shard->loop_idx++;
                    continue;
                }
                shard->loop_idx++;
            } else {
                if (!FLAGS_trace_wrap_main && shard->main_exhausted) {
                    continue;
                }
                op_opt = get_main_operation(shard->main_idx, shard->op_rank, FLAGS_trace_wrap_main);
                if (!op_opt) {
                    shard->main_exhausted = true;
                    continue;
                }
                shard->main_idx++;
            }

            int slot = -1;
            if (!unlimited_inflight) {
                slot = find_slot(*shard, now_cycles);
                if (slot < 0) {
                    break;
                }
            }

            rte_mbuf *tx_pkt = rte_pktmbuf_alloc(ctx.mempools[shard->queue_id]);
            if (!tx_pkt) {
                break;
            }

            Operation op = *op_opt;
            const uint16_t dst_port =
                protocol::dst_port(FLAGS_dst_port_base, static_cast<uint16_t>(FLAGS_num_server_readers), op.key);
            const uint16_t src_port = pick_src_port(*shard, dst_port);

            auto *kv = fill_packet(tx_pkt, src_port, dst_port);
            op.to_packet(kv);
            kv->queue_id = shard->queue_id;
            kv->timestamp = cycles_to_ns(cycles_now());
            if (!unlimited_inflight) {
                kv->slot_id = static_cast<uint16_t>(slot);
                shard->slot_ts_cycles[slot] = now_cycles;
            } else {
                kv->slot_id = 0;
            }

            const uint16_t sent = rte_eth_tx_burst(ctx.port_id, shard->queue_id, &tx_pkt, 1);
            if (sent == 1) {
                shard->sent++;
                if (!unlimited_inflight) {
                    shard->inflight++;
                }
            } else {
                if (!unlimited_inflight && slot >= 0) {
                    shard->slot_ts_cycles[slot] = 0;
                }
                rte_pktmbuf_free(tx_pkt);
                break;
            }

            if (unlimited_inflight) {
                break;
            }
        }
    }

    return 0;
}

int start_shards() {
    for (uint32_t i = 0; i < ctx.shards.size(); ++i) {
        const int ret = rte_eal_remote_launch(shard_loop, &ctx.shards[i], ctx.shards[i].lcore_id);
        if (ret != 0) {
            log_error("Failed to launch shard %u on lcore %u: %s", i, ctx.shards[i].lcore_id,
                      rte_strerror(-ret));
            ctx.stop.store(true, std::memory_order_relaxed);
            for (uint32_t j = 0; j < i; ++j) {
                rte_eal_wait_lcore(ctx.shards[j].lcore_id);
            }
            return ret;
        }
    }
    return 0;
}

void stop_shards() {
    ctx.stop.store(true, std::memory_order_relaxed);
    unsigned lcore_id;
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (std::find(ctx.shard_lcores.begin(), ctx.shard_lcores.end(), lcore_id) ==
            ctx.shard_lcores.end()) {
            continue;
        }
        rte_eal_wait_lcore(lcore_id);
    }
}

void print_latencies() {
    if (ctx.shards.empty()) {
        return;
    }
    const auto &samples = ctx.shards[FLAGS_latency_queue_id % ctx.shards.size()].latency_samples;
    if (samples.empty()) {
        log_info("No latency samples captured.");
        return;
    }
    std::vector<uint64_t> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double q) {
        const double pos = q * (sorted.size() - 1);
        const size_t idx = static_cast<size_t>(std::ceil(pos));
        return sorted[idx];
    };
    log_info(
        "Rank %zu latency percentiles (ns): p50=%lu p90=%lu p99=%lu p99.9=%lu p99.99=%lu "
        "p99.999=%lu p99.9999=%lu",
        global_rank(), pct(0.50), pct(0.90), pct(0.99), pct(0.999), pct(0.9999), pct(0.99999),
        pct(0.999999));
}

int init_shard_layout(uint32_t num_shards) {
    unsigned lcore_id;
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (ctx.shard_lcores.size() >= num_shards) {
            break;
        }
        ctx.shard_lcores.push_back(lcore_id);
    }
    if (ctx.shard_lcores.size() < num_shards) {
        log_error("Not enough slave lcores for %u shards (found %zu)", num_shards, ctx.shard_lcores.size());
        return -EINVAL;
    }

    ctx.shards.resize(num_shards);
    std::vector<uint16_t> queue_sockets(num_shards, rte_socket_id());
    for (uint32_t i = 0; i < num_shards; ++i) {
        ctx.shards[i].queue_id = static_cast<uint16_t>(i);
        ctx.shards[i].lcore_id = ctx.shard_lcores[i];
        ctx.shards[i].socket_id = rte_lcore_to_socket_id(ctx.shard_lcores[i]);
        ctx.shards[i].op_rank = global_rank() * num_shards + i;
        ctx.shards[i].slot_ts_cycles.resize(ctx.num_inflight, 0);
        if (ctx.shards[i].queue_id == FLAGS_latency_queue_id) {
            ctx.shards[i].latency_samples.reserve(FLAGS_ops_per_thread ? FLAGS_ops_per_thread : 1'000'000);
        }
        queue_sockets[i] = ctx.shards[i].socket_id;
    }

    int ret = create_mempools(num_shards, queue_sockets);
    if (ret != 0) {
        return ret;
    }
    return setup_device(ctx.port_id, static_cast<uint16_t>(num_shards), queue_sockets);
}

}  // namespace

/* Required stubs for the benchmark framework (unused in this target). */
void target_thread_exit(std::size_t) {}
void target_thread_init(std::size_t) {}
OperationResult target_do_operation(std::size_t, std::size_t, const Operation &) {
    return {};
}
std::vector<std::size_t> target_check_for_replies(std::size_t) {
    return {};
}

int target_on_benchmark_ready(int argc, char **argv) {
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        log_error("Failed to init RTE EAL: %s", rte_strerror(rte_errno));
        return ret;
    }
    argc -= ret;
    argv += ret;
    ret = 0;

    gflags::ParseCommandLineFlags(&argc, &argv, true);
    if (FLAGS_trace_no_wrap_main && FLAGS_trace_wrap_main) {
        log_error("Cannot set both --trace-wrap-main and --trace-no-wrap-main");
        return -EINVAL;
    }
    if (FLAGS_trace_no_wrap_main) {
        FLAGS_trace_wrap_main = false;
    }
    ctx.num_shards = FLAGS_num_shard_threads ? FLAGS_num_shard_threads : FLAGS_num_tx_threads;
    ctx.num_inflight = FLAGS_num_inflight;

    ctx.timer_hz = rte_get_timer_hz();
    ctx.timeout_cycles = (FLAGS_starvation_time * ctx.timer_hz) / 1000000ull;

    if (ctx.num_shards == 0) {
        log_error("num_shard_threads/num_tx_threads must be greater than zero");
        return -EINVAL;
    }
    if (FLAGS_latency_queue_id >= ctx.num_shards) {
        log_info("latency_queue_id %u exceeds shard count %u, using queue 0", FLAGS_latency_queue_id, ctx.num_shards);
        FLAGS_latency_queue_id = 0;
    }
    set_manual_rank(FLAGS_rank_id, FLAGS_world_size);
    set_operation_rank_span(ctx.num_shards * global_world_size());

    if ((ret = load_trace_file_or_fail()) != 0) {
        return ret;
    }
    log_info("Rank %zu/%zu initialized trace", global_rank(), global_world_size());
    if ((ret = resolve_port_id(ctx.port_id)) != 0) {
        return ret;
    }

    if ((ret = configure_addresses()) != 0) {
        return ret;
    }

    if (!FLAGS_src_mac.empty()) {
        if (!parse_mac(FLAGS_src_mac, &ctx.src_mac)) {
            log_error("Failed to parse src_mac flag");
            return -EINVAL;
        }
    } else if (rte_eth_macaddr_get(ctx.port_id, &ctx.src_mac) != 0) {
        log_error("Failed to get MAC address for port %u", ctx.port_id);
        return -EINVAL;
    }

    if (!FLAGS_dst_mac.empty()) {
        if (!parse_mac(FLAGS_dst_mac, &ctx.dst_mac)) {
            log_error("Failed to parse dst_mac flag");
            return -EINVAL;
        }
    } else {
        ctx.dst_mac.addr_bytes[0] = 0xa0;
        ctx.dst_mac.addr_bytes[1] = 0x88;
        ctx.dst_mac.addr_bytes[2] = 0xc2;
        ctx.dst_mac.addr_bytes[3] = 0xd3;
        ctx.dst_mac.addr_bytes[4] = 0x2c;
        ctx.dst_mac.addr_bytes[5] = static_cast<std::uint8_t>(0xa8 + ctx.port_id);
    }

    ret = init_shard_layout(ctx.num_shards);
    if (ret != 0) {
        return ret;
    }

    start_barrier(FLAGS_barrier_file);

    ::signal(SIGINT, signal_handler);
    ::signal(SIGTERM, signal_handler);

    ret = start_shards();
    if (ret != 0) {
        stop_shards();
        return ret;
    }

    log_info("Target ready with %u shard threads, inflight %u, timeout %u us", ctx.num_shards, ctx.num_inflight,
             FLAGS_starvation_time);
    return 0;
}

void target_on_benchmark_run() {
    auto last = std::chrono::steady_clock::now();
    std::vector<uint64_t> last_sent(ctx.shards.size(), 0);
    std::vector<uint64_t> last_recv(ctx.shards.size(), 0);

    while (!ctx.stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto now = std::chrono::steady_clock::now();
        const double delta_s = std::chrono::duration<double>(now - last).count();
        last = now;

        uint64_t total_sent = 0, total_recv = 0, total_inflight = 0, total_timeouts = 0;
        uint64_t delta_sent = 0, delta_recv = 0;
        for (std::size_t i = 0; i < ctx.shards.size(); ++i) {
            const auto &s = ctx.shards[i];
            total_sent += s.sent;
            total_recv += s.received;
            total_inflight += s.inflight;
            total_timeouts += s.timed_out;
            delta_sent += (s.sent - last_sent[i]);
            delta_recv += (s.received - last_recv[i]);
            last_sent[i] = s.sent;
            last_recv[i] = s.received;
        }

        const double send_rate = delta_s > 0 ? delta_sent / delta_s : 0.0;
        const double recv_rate = delta_s > 0 ? delta_recv / delta_s : 0.0;

        std::cout << TARGET_STR << "inflight=" << total_inflight << " sent=" << total_sent << " recv=" << total_recv
                  << " timeouts=" << total_timeouts << " send/s=" << send_rate << " recv/s=" << recv_rate << "\n";

        if (FLAGS_ops_per_thread && all_shards_finished()) {
            ctx.stop.store(true, std::memory_order_relaxed);
        }
    }

    stop_shards();
    print_latencies();

    rte_eth_dev_stop(ctx.port_id);
    rte_eth_dev_close(ctx.port_id);
    for (auto *mp : ctx.mempools) {
        if (mp) {
            rte_mempool_free(mp);
        }
    }
    rte_eal_cleanup();
    log_info("Target shutdown complete");
}
