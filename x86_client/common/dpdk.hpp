#pragma once

#include <gflags/gflags.h>

#include <stdio.h>
#include <signal.h>

#include <rte_arp.h>
#include <rte_cycles.h>
#include <rte_debug.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_memory.h>
#include <rte_per_lcore.h>
#include <rte_udp.h>

#include "protocol.hpp"

#define info___(fmt, ...)                                                       \
do {                                                                            \
    printf("[%s:%u] " fmt "\n",                                                 \
        __FILE__, __LINE__, ##__VA_ARGS__);                                     \
} while (0)

#define error__(errval, fmt, ...)                                               \
do {                                                                            \
    printf("[%s:%u] " fmt " (%d)\n",                                            \
        __FILE__, __LINE__, ##__VA_ARGS__, errval);                             \
    err = errval;                                                               \
} while (0)

#define eal_err(fmt, ...)                                                       \
do {                                                                            \
    error__(-rte_errno, fmt " EAL error: %s",                                   \
        ##__VA_ARGS__, rte_strerror(rte_errno));                                \
} while (0)

#define pkt_extract(raw, type)                                                  \
({                                                                              \
    type *t = (type *)raw;                                                      \
    raw += sizeof(type);                                                        \
    t;                                                                          \
})

#define MAX_DEV_NAME        16
#define MAX_PCI_NAME        16
#define MAX_POOL_NAME_LEN   16

/**
 * A structure describing a generic device.
 */
struct rte_device {
    RTE_TAILQ_ENTRY(rte_device) next;   /**< Next device */
    const char *name;                   /**< Device name */
    const char *bus_info;               /**< Device bus specific information */
    const struct rte_driver *driver;    /**< Driver assigned after probing */
    const struct rte_bus *bus;          /**< Bus handle assigned on scan */
    int numa_node;                      /**< NUMA node connection */
    struct rte_devargs *devargs;        /**< Arguments for latest probing */
};

int get_port_id(const char *dev_name, uint32_t num_threads);
bool parse_mac(const std::string& mac_str, rte_ether_addr* out);
int setup_device(uint32_t num_rx_threads, uint32_t num_tx_threads);
void teardown_device(void);

struct rte_mbuf *alloc_tx_pkt(uint16_t queue_id);
void fill_ether(struct rte_ether_hdr *eth, struct rte_ether_addr *src_mac, struct rte_ether_addr *dst_mac);
void fill_ipv4(struct rte_ipv4_hdr *ipv4, rte_be32_t src_ip, rte_be32_t dst_ip, uint16_t udp_payload_len);
void fill_udp(struct rte_udp_hdr *udp, uint16_t src_port, uint16_t dst_port, uint16_t udp_payload_len);
protocol::KVPacket *fill_pkt(struct rte_mbuf *pkt, struct rte_ether_addr *src_mac, struct rte_ether_addr *dst_mac,
                             std::uint32_t src_ip, std::uint32_t dst_ip, std::uint16_t src_port, std::uint16_t dst_port);
