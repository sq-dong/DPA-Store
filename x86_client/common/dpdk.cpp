
#include "dpdk.hpp"

struct dpdk_ctx {
    uint16_t num_tx_threads;
    uint16_t num_rx_threads;
    uint16_t nr_pools;
    uint16_t port_id;
    char pci[MAX_DEV_NAME];
    struct rte_mempool **rx_pools;
    struct rte_mempool **tx_pools;
} dpdk_ctx;


/*
 *
 * Constructors.
 *
 */
static int create_mbuf_pools(struct rte_mempool **pools, uint32_t num_threads)
{
    char pool_name[MAX_POOL_NAME_LEN];
    int err = 0;
    int i;

    for (i = 0; i < num_threads; i++) {
        snprintf(pool_name, MAX_POOL_NAME_LEN, "pool_%03u", dpdk_ctx.nr_pools++);
        pools[i] = rte_pktmbuf_pool_create(pool_name,
                                           4096, 500, 0,
                                           RTE_MBUF_DEFAULT_BUF_SIZE,
                                           rte_socket_id());
        if (!pools[i]) {
            eal_err("Cannot create mbuf pool.");
            goto free_pools;
        }
    }

    info___("Allocated mbuf pools.");

    goto out;

free_pools:
    while (i--)
        rte_mempool_free(pools[i]);

out:
    return err;
}

static int setup_rx_queues(uint32_t num_threads)
{
    int err = 0;
    int i;

    dpdk_ctx.num_rx_threads = num_threads;

    dpdk_ctx.rx_pools = (struct rte_mempool **)calloc(num_threads, sizeof(struct rte_mempool *));
    if (!dpdk_ctx.rx_pools) {
        error__(-ENOMEM, "Failed to allocate mempool array.");

        goto out;
    }

    if ((err = create_mbuf_pools(dpdk_ctx.rx_pools, num_threads)))
        goto destroy_array;

    for (i = 0; i < num_threads; i++) {
        if (rte_eth_rx_queue_setup(dpdk_ctx.port_id, i, 1024,
                                   rte_eth_dev_socket_id(dpdk_ctx.port_id),
                                   NULL, dpdk_ctx.rx_pools[i])) {
            eal_err("Failed to set up RX queue #%d.", i);

            goto out;
        }
    }

    goto out;

destroy_array:
    free(dpdk_ctx.rx_pools);

out:
    return err;
}

static int setup_tx_queues(uint32_t num_threads)
{
    int err = 0;
    int i;

    dpdk_ctx.num_tx_threads = num_threads;

    dpdk_ctx.tx_pools = (struct rte_mempool **)calloc(num_threads, sizeof(struct rte_mempool *));
    if (!dpdk_ctx.tx_pools) {
        error__(-ENOMEM, "Failed to allocate mempool array.");

        goto out;
    }

    if ((err = create_mbuf_pools(dpdk_ctx.tx_pools, num_threads)))
        goto destroy_array;

    for (i = 0; i < num_threads; i++) {
        if (rte_eth_tx_queue_setup(dpdk_ctx.port_id, i, 1024,
                                   rte_eth_dev_socket_id(dpdk_ctx.port_id),
                                   NULL)) {
            eal_err("Failed to set up RX queue #%d.", i);

            goto out;
        }
    }

    goto out;

destroy_array:
    free(dpdk_ctx.tx_pools);

out:
    return err;
}


/*
 *
 * Destructors.
 *
 */
static void destroy_mbuf_pools(struct rte_mempool **pools, uint32_t num_threads)
{
    int i = num_threads;

    while (i--)
        rte_mempool_free(pools[i]);
}

void destroy_rx_queues()
{
    destroy_mbuf_pools(dpdk_ctx.rx_pools, dpdk_ctx.num_rx_threads);
    free(dpdk_ctx.rx_pools);
}

void destroy_tx_queues()
{
    destroy_mbuf_pools(dpdk_ctx.tx_pools, dpdk_ctx.num_tx_threads);
    free(dpdk_ctx.tx_pools);
}


/*
 *
 * Callables.
 * Recommended order:
 *  - rte_eal_init
 *  - get_port_id
 *  - setup_device
 *
 */
int get_port_id(const char *dev_name, uint32_t num_threads)
{
    struct rte_eth_dev_info dev_info;
    char command[80];
    char output[MAX_PCI_NAME];
    FILE *file;
    int err = 0;
    uint16_t port_id;
    char mlx_idx = dev_name[strlen(dev_name) - 1];

    if (mlx_idx < '0' || mlx_idx > '9') {
        error__(-EINVAL, "Bad device name. Expected something like 'mlx5_x', but got %s.",
                dev_name);

        return err;
    }

    snprintf(command, sizeof(command),
             "lspci | grep Mellanox | grep 00.%c | cut -d ' ' -f 1 | tr -d '\n'",
             mlx_idx);

    file = popen(command, "r");
    if (!file) {
        error__(-EIO, "Failed to get PCI address via lspci. "
                "Try yourself via 'lspci | grep Mellanox'.");

        return err;
    }

    if (!fgets(output, sizeof(output), file)) {
        error__(-EIO, "Failed to get output from \"%s\"", command);

        return err;
    }

    snprintf(dpdk_ctx.pci, MAX_PCI_NAME, "%s", output);

    RTE_ETH_FOREACH_DEV(port_id) {
        if ((err = rte_eth_dev_info_get(port_id, &dev_info))) {
            error__(-err, "Failed to get dev info.");
            continue;
        }

        /* Filter SF. */
        if (!strstr(dev_info.driver_name, "pci")) {
            info___("Device %s has bad driver (%s). Skipping.",
                    dev_info.device->name, dev_info.driver_name);
            continue;
        }

        if (!strstr(dev_info.device->name, dpdk_ctx.pci)) {
            info___("This is not the port we are looking for (%s != %s).",
                    dev_info.device->name, dpdk_ctx.pci);
            continue;
        }

        info___("Device info: {device: {name %s, bus %s, if %u}, "
                "mtu {min: %u, max %u}, pmd: %s, rx %u, tx %u}.",
                dev_info.device->name, dev_info.device->bus_info,
                dev_info.if_index, dev_info.min_mtu, dev_info.max_mtu,
                dev_info.driver_name, dev_info.max_rx_queues,
                dev_info.max_tx_queues);

        if (num_threads > dev_info.max_rx_queues || num_threads > dev_info.max_tx_queues) {
            error__(-EINVAL, "Device only supports %u/%u RX/TX queues respectively. "
                    "Applications asked for %u.",
                    dev_info.max_rx_queues, dev_info.max_tx_queues,
                    num_threads);
		    return err;
        }

        return port_id;
    }

    error__(-ENODEV, "No suitable device found.");

    return err;
}

bool parse_mac(const std::string& mac_str, rte_ether_addr* out) {
    unsigned int bytes[6];
    if (std::sscanf(mac_str.c_str(), "%x:%x:%x:%x:%x:%x",
                    &bytes[0], &bytes[1], &bytes[2],
                    &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return false;
    }

    for (int i = 0; i < 6; ++i) {
        out->addr_bytes[i] = static_cast<std::uint8_t>(bytes[i]);
    }
    return true;
}

int setup_device(uint32_t num_rx_threads, uint32_t num_tx_threads)
{
    struct rte_eth_conf port_conf = {0};
    int err = 0;

    port_conf.txmode.offloads = RTE_ETH_RX_OFFLOAD_UDP_CKSUM |
                                RTE_ETH_RX_OFFLOAD_IPV4_CKSUM |
                                0;
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    port_conf.rx_adv_conf.rss_conf = (struct rte_eth_rss_conf) {
        .rss_key = NULL,
        .rss_key_len = 0,
        .rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP
    };

    if ((err = rte_eth_dev_configure(dpdk_ctx.port_id, num_rx_threads,
                                     num_tx_threads, &port_conf))) {
        error__(-err, "Failed to configure device #%u.", dpdk_ctx.port_id);

        goto out;
    }

    if ((err = setup_rx_queues(num_rx_threads)))
        goto out;

    if ((err = setup_tx_queues(num_tx_threads)))
        goto destroy_rx;

    if ((err = rte_eth_dev_start(dpdk_ctx.port_id))) {
        error__(-err, "Failed to start ethernet device #%u.", dpdk_ctx.port_id);

        goto destroy_tx;
    }

    goto out;

destroy_tx:
    destroy_tx_queues();

destroy_rx:
    destroy_rx_queues();

out:
    return err;
}

void teardown_device(void)
{
    rte_eth_dev_stop(dpdk_ctx.port_id);
    rte_eth_dev_close(dpdk_ctx.port_id);
    destroy_tx_queues();
    destroy_rx_queues();
}

struct rte_mbuf *alloc_tx_pkt(uint16_t queue_id)
{
    return rte_pktmbuf_alloc(dpdk_ctx.tx_pools[queue_id]);
}

void fill_ether(struct rte_ether_hdr *eth, struct rte_ether_addr *src_mac, struct rte_ether_addr *dst_mac)
{
	rte_ether_addr_copy(dst_mac, &eth->dst_addr);
	rte_ether_addr_copy(src_mac, &eth->src_addr);
	eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
}

void fill_ipv4(struct rte_ipv4_hdr *ipv4, rte_be32_t src_ip, rte_be32_t dst_ip, uint16_t udp_payload_len)
{
	ipv4->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) >> 2);
	ipv4->type_of_service = 0;
	ipv4->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + udp_payload_len);
	// ipv4->packet_id = rte_cpu_to_be_16(seq_nr_lsb);
	ipv4->fragment_offset = 0;
	ipv4->time_to_live = 64;
	ipv4->next_proto_id = IPPROTO_UDP;
	ipv4->src_addr = src_ip;
	ipv4->dst_addr = dst_ip;

	/* Offloaded to NIC. */
	ipv4->hdr_checksum = 0;
}

void fill_udp(struct rte_udp_hdr *udp, rte_be16_t src_port, rte_be16_t dst_port, uint16_t udp_payload_len)
{
    udp->src_port = src_port;
    udp->dst_port = dst_port;
    udp->dgram_len = rte_cpu_to_be_16(sizeof(*udp) + udp_payload_len);

    /* Offloaded to NIC. */
    udp->dgram_cksum = 0;
}

protocol::KVPacket *fill_pkt(struct rte_mbuf *pkt,
                             struct rte_ether_addr *src_mac,
                             struct rte_ether_addr *dst_mac,
                             std::uint32_t src_ip, std::uint32_t dst_ip,
                             rte_be16_t src_port, rte_be16_t dst_port) {
    struct rte_ether_hdr *tx_eth;
    struct rte_ipv4_hdr *tx_ipv4;
    struct rte_udp_hdr *tx_udp;
    struct protocol::KVPacket *request;
    const uint16_t tx_base_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr);
    std::byte *tx_raw = rte_pktmbuf_mtod(pkt, std::byte *);

    /* Walk packet. */
    tx_eth = pkt_extract(tx_raw, struct rte_ether_hdr);
    tx_ipv4 = pkt_extract(tx_raw, struct rte_ipv4_hdr);
    tx_udp = pkt_extract(tx_raw, struct rte_udp_hdr);
    request = pkt_extract(tx_raw, struct protocol::KVPacket);

    /* Fill frames/headers. */
    fill_ether(tx_eth, src_mac, dst_mac);
    fill_ipv4(tx_ipv4, src_ip, dst_ip, sizeof(protocol::KVPacket));
    fill_udp(tx_udp, src_port, dst_port, sizeof(protocol::KVPacket));

    /* DPDK mbuf metadata. */
    pkt->data_len = tx_base_len + sizeof(protocol::KVPacket);
    pkt->pkt_len = tx_base_len + sizeof(protocol::KVPacket);
    pkt->ol_flags = RTE_MBUF_F_TX_IPV4 |
                       RTE_MBUF_F_TX_IP_CKSUM |
                       RTE_MBUF_F_TX_UDP_CKSUM;
    pkt->l2_len = sizeof(struct rte_ether_hdr);
    pkt->l3_len = sizeof(struct rte_ipv4_hdr);

    /* Return request header to be filled by caller. */
    return request;
}
