#pragma once

#include "common/dpastore_common_config.h"

// --- Packet field offsets ---
#define PKT_SRC_MAC(data)        ((data) + 0)
#define PKT_DST_MAC(data)        ((data) + 6)
#define PKT_LENGTH(data)         ((data) + 12)
#define PKT_SRC_IP(data)         ((data) + 14 + 12)
#define PKT_DST_IP(data)         ((data) + 14 + 12 + 4)
#define PKT_UDP_SRC(data)        ((data) + 14 + 20 + 0)
#define PKT_UDP_DST(data)        ((data) + 14 + 20 + 2)
#define PKT_UDP(data)            ((data) + 14 + 20) // Start of UDP header (after Eth + IP)
#define PKT_UDP_SRC(data)        ((data) + 14 + 20 + 0)
#define PKT_UDP_DST(data)        ((data) + 14 + 20 + 2)
#define PKT_UDP_LEN(data)        ((data) + 14 + 20 + 4)
#define PKT_UDP_CHECK(data)      ((data) + 14 + 20 + 6)
#define PKT_CONTENT1(data)       ((data) + 14 + 28)
#define PKT_CONTENT2(data)       ((data) + 14 + 28 + 8)
#define PKT_CONTENT4(data)       ((data) + 14 + 28 + 24)
#define PKT_CONTENT5(data)       ((data) + 14 + 28 + 32)

// --- Utility functions ---

void print_hex(const char *s, int length)
{
    for (int i = 0; i < length; i++)
        flexio_dev_print("%02x", (unsigned int)*(s + i));
}

void print_string(const char *s, int length)
{
    for (int i = 0; i < length; i++)
        flexio_dev_print("%c", *(s + i));
}

// Tree printing function removed - will be added when tree implementation is ready


#include "common/dpastore_common.h"

static inline void print_dpastore_packet_content(const DPAStorePacket *packet)
{
    LOG_I("--- DPAStorePacket Content ---\n");
    uint8_t op_type = PACKET_GET_OP_TYPE(packet->control);
    uint8_t status = PACKET_GET_STATUS(packet->control);
    uint8_t hop_count = PACKET_GET_HOP_COUNT(packet->control);
    LOG_I("control: 0x%02x (op_type: %u, status: %u, hop_count: %u)\n", 
          packet->control, op_type, status, hop_count);
    LOG_I("request_id: %u\n", packet->request_id);
    LOG_I("key: %llu\n", packet->key);
    LOG_I("value: %llu\n", packet->value);
    LOG_I("------------------------------\n");
}

// --- Packet field printing macros ---

#define PRINT_PACKET_FIELD(label, ptr, len) \
    LOG_I(label);                    \
    print_hex(ptr, len);                    \
    flexio_dev_print("\n");

#define PRINT_PACKET_CONTENT(idx, ptr) \
    PRINT_PACKET_FIELD("Packet content " #idx ": ", (char*)&(long long){cpu_to_be64(*(long long*)(ptr))}, 8)

// --- Packet pretty printer ---

static inline void print_packet(char *data, uint32_t data_sz)
{
    if (CUR_LOG_LEVEL < LOG_LEVEL_INFO)
        return;


    PRINT_PACKET_FIELD("Packet src mac: ", PKT_SRC_MAC(data), 6)
    PRINT_PACKET_FIELD("Packet dst mac: ", PKT_DST_MAC(data), 6)
    PRINT_PACKET_FIELD("Packet length: ", PKT_LENGTH(data), 2)
    PRINT_PACKET_FIELD("Packet src ip: ", PKT_SRC_IP(data), 4)
    PRINT_PACKET_FIELD("Packet dst ip: ", PKT_DST_IP(data), 4)

    // UDP fields
    const uint16_t raw_udp_src = *(uint16_t*)(PKT_UDP_SRC(data));
    const uint16_t raw_udp_dst = *(uint16_t*)(PKT_UDP_DST(data));
    const uint16_t raw_udp_len = *(uint16_t*)(PKT_UDP_LEN(data));
    const uint16_t raw_udp_check = *(uint16_t*)(PKT_UDP_CHECK(data));

    LOG_I("Packet udp source (raw): ");
    flexio_dev_print("0x%04x\n", raw_udp_src);
    LOG_I("Packet udp source (host): ");
    flexio_dev_print("%hu\n", be16_to_cpu(raw_udp_src));

    LOG_I("Packet udp dest (raw): ");
    flexio_dev_print("0x%04x\n", raw_udp_dst);
    LOG_I("Packet udp dest (host): ");
    flexio_dev_print("%hu\n", be16_to_cpu(raw_udp_dst));

    LOG_I("Packet udp length (raw): ");
    flexio_dev_print("0x%04x\n", raw_udp_len);
    LOG_I("Packet udp length (host): ");
    flexio_dev_print("%hu\n", be16_to_cpu(raw_udp_len));

    LOG_I("Packet udp check (raw): ");
    flexio_dev_print("0x%04x\n", raw_udp_check);
    LOG_I("Packet udp check (host): ");
    flexio_dev_print("%hu\n", be16_to_cpu(raw_udp_check));

    // Content fields
    DPAStorePacket *dpa_packet = (DPAStorePacket*)PKT_CONTENT1(data);
    print_dpastore_packet_content(dpa_packet);
}
