#pragma once

// =============================================================================
// CONFIGURE FOR YOUR SITE: the values below are baked for the authors' lab
// machines (oscar = server, grobi = client).  To run on a different setup,
// edit the CLIENT_* / SERVER_* defines at the bottom of this file (or define
// your own *_IP / *_IP_HEX / *_MAC / *_MAC_HEX block and re-point them).
//
// IP format:  dotted-decimal string, e.g. "10.0.1.11"
// IP_HEX:     same address as a 32-bit big-endian hex literal (0x0a00010b)
// MAC_HEX:    48-bit hex literal, e.g. 0xaabbccddeeff
// =============================================================================

// Lab machine: dbar-a (enp65s0f1np1, a0:88:c2:d3:20:31, 10.0.1.11/24)
//   host NIC MAC : a0:88:c2:d3:20:31  (= 0xa088c2d32031)
//   DPU port MAC : a0:88:c2:d3:20:41  (= 0xa088c2d32041)  — visible only when BF in DPU mode
// Lab machine: dbar-b (enp65s0f1np1, a0:88:c2:d3:2c:a9, 10.0.1.12/24)

#define DBAR_A_IP "10.0.1.11"
#define DBAR_A_IP_HEX 0x0a00010b
#define DBAR_A_MAC "a0:88:c2:d3:20:31"
#define DBAR_A_MAC_HEX 0xa088c2d32031
#define DBAR_A_DPU_MAC_HEX 0xa088c2d32041

#define DBAR_B_IP "10.0.1.12"
#define DBAR_B_IP_HEX 0x0a00010c
#define DBAR_B_MAC "a0:88:c2:d3:2c:a9"
#define DBAR_B_MAC_HEX 0xa088c2d32ca9


#define AA_OSCAR_IP "137.226.133.150"
#define AA_OSCAR_IP_HEX 0x89e28596
#define AA_OSCAR_MAC "58:a2:e1:2f:9e:60"
#define AA_OSCAR_MAC_HEX 0x58a2e12f9e60

#define AA_GROBI_IP "137.226.133.165"
#define AA_GROBI_IP_HEX 0x89e285a5
#define AA_GROBI_MAC "58:a2:e1:2f:9e:76"
#define AA_GROBI_MAC_HEX 0x58a2e12f9e76

// -----------------------------------------------------------------------------
// Local cluster (aligned with PD3 machines; Ethernet data plane for DPA-Store)
//   Server host : memserver16
//     BF-3 PCI b1:00  → mlx5_2/ibs5f0 (IB, PD3) + mlx5_3/ens5f1np1 (Eth, DPA-Store)
//     CX-5  PCI 98:00 → mlx5_0/ibs6f0 + mlx5_1/ens6f1np1  — NOT usable for DPA
//   Client host : bfhost_72  — BF-3 port1 Ethernet ens2f1np1 / mlx5_1
// Subnet 10.0.1.0/24 is dedicated to DPA-Store UDP; do NOT reuse PD3 IPoIB
// 10.10.1.0/24 or management 10.208.130.0/24 for the DPDK/FlexIO path.
//
// CLIENT_MAC verified: ens2f1np1 → 7c:8c:09:bf:34:39
// CLIENT_DPU_MAC_HEX is only needed for open-loop dpa_client+trafgen (-d).
// -----------------------------------------------------------------------------
#define MS16_IP "10.0.1.12"
#define MS16_IP_HEX 0x0a00010c
#define MS16_MAC "7c:8c:09:bf:36:73"
#define MS16_MAC_HEX 0x7c8c09bf3673

#define BF72_IP "10.0.1.11"
#define BF72_IP_HEX 0x0a00010b
#define BF72_MAC "7c:8c:09:bf:34:39"
#define BF72_MAC_HEX 0x7c8c09bf3439
// TODO: replace after measuring DPU-port MAC on bfhost_72 in DPU mode
#define BF72_DPU_MAC_HEX 0x7c8c09bf3439

// CONFIGURE: re-point these to your client/server machine if not running
// on the lab setup above.
//
//   CLIENT_*           : the DPA-client / multiclient host (where load is generated)
//   SERVER_*           : the DPA-Store server host
//   *_MAC_HEX          : host NIC MAC
//   CLIENT_DPU_MAC_HEX : DPU-port MAC on the DPA-client host (BF in DPU mode)
//   *_DEVICE_NAME      : FlexIO/RDMA device name (ibv_devinfo) used by the
//                        server / dpa_client host process
#define CLIENT_IP            BF72_IP
#define CLIENT_IP_HEX        BF72_IP_HEX
#define CLIENT_MAC_HEX       BF72_MAC_HEX
#define CLIENT_DPU_MAC_HEX   BF72_DPU_MAC_HEX
#define CLIENT_DEVICE_NAME   "mlx5_1"

#define SERVER_IP            MS16_IP
#define SERVER_IP_HEX        MS16_IP_HEX
#define SERVER_MAC_HEX       MS16_MAC_HEX
#define SERVER_DEVICE_NAME   "mlx5_3"

#define DPASTORE_READER_THREAD_COUNT 176
#define DPASTORE_UPDATER_THREAD_COUNT 0

#define DPASTORE_CLIENT_THREAD_COUNT DPASTORE_READER_THREAD_COUNT

#ifndef DPASTORE_ENABLE_HOT_CACHE
#define DPASTORE_ENABLE_HOT_CACHE 1
#endif

#ifndef DPASTORE_RANGE_REQUEST  // We can add second response queues for range request in future, but now we extend default ones.
#define DPASTORE_RANGE_REQUEST 0
#endif

// UDP port map. IMPORTANT: reader/updater bases must NOT depend on the current
// DPASTORE_READER_THREAD_COUNT. Fig.9 sweeps rebuild the server with different
// traverser counts; if the base moves with N, a prebuilt multiclient still
// targets the old ports and almost all requests time out.
#define DPASTORE_UDPPORT_CLIENT             23998
#ifndef DPASTORE_UDP_PORT_BUDGET
#define DPASTORE_UDP_PORT_BUDGET            256   /* ≥ max usable DPA threads */
#endif
#define DPASTORE_UDPPORT_NIC                (DPASTORE_UDPPORT_CLIENT + DPASTORE_UDP_PORT_BUDGET)
#define DPASTORE_UDPPORT_READER             (DPASTORE_UDPPORT_NIC + 1)
#define DPASTORE_UDPPORT_UPDATER            (DPASTORE_UDPPORT_READER + DPASTORE_UDP_PORT_BUDGET)

// Special ID for distinguishing requests originating from readers vs. updaters
// Ensure this value doesn't overlap with valid thread indices (0 to N-1)
#define DPASTORE_READER_THREAD_ID_SENTINEL 0xFFFFFFFF // Example value
