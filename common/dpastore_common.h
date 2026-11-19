#ifndef DPASTORE_COMMON_H
#define DPASTORE_COMMON_H

#include "dpastore_common_config.h" // Keep this include
#include "../server/shared/hotcache.h"

// --- Performance and Response Configuration ---
// These compile-time flags control packet response behavior and performance measurement

// Response mode constants
#define RESPONSE_NONE        0
#define RESPONSE_SEND_DISCARD 1
#define RESPONSE_SEND_EXPECT 2

// RESPONSE_MODE: Controls packet response behavior
// RESPONSE_NONE: No response packets sent back
// RESPONSE_SEND_DISCARD: Response sent but client doesn't expect it (goes nowhere)
// RESPONSE_SEND_EXPECT: Response sent and client expects it
#ifndef DPASTORE_RESPONSE_MODE
#define DPASTORE_RESPONSE_MODE RESPONSE_SEND_EXPECT  // Default: full response mode
#endif

// PERFORMANCE_COUNTING: Controls packet counting and periodic statistics printing
// 0 = No packet counting or periodic printing
// 1 = Enable packet counting and periodic throughput statistics
#ifndef DPASTORE_PERFORMANCE_COUNTING
#define DPASTORE_PERFORMANCE_COUNTING 1  // Default: enabled
#endif

// Test mode for direct packet return with counting - compile-time switch
#ifndef DPASTORE_TEST_MODE_DIRECT_RETURN
#define DPASTORE_TEST_MODE_DIRECT_RETURN 0  // Default: disabled (set to 1 to enable direct test response)
#endif // DPASTORE_TEST_MODE_DIRECT_RETURN

#include <stdint.h> // For uint*_t types
#include <stddef.h> // For size_t

#ifdef __DEVICE__
#include "wrapper_flexio_device.h" // For flexio_uintptr_t on device
#elif defined(__HOST__)
#include "wrapper_flexio.hpp" // For flexio_uintptr_t on host (assuming it's defined here)
#endif

#define DPASTORE_STAMP 0x1f2f3f4f5f6f7f8f

// #define TREE_KEY_TYPE uint64_t
// typedef struct {
//     TREE_KEY_TYPE key;
//     void* value; // Can be actual value (leaf) or TreeNode* (inner)
// } KVPair;

#ifndef __MULTICLIENT_CLIENT__
#ifndef __CLIENT__
// Struct to pass test data buffer info via RPC (used by host and device)
typedef struct {
    flexio_uintptr_t addr;
    size_t size;
} test_data_buffer_info;

// Struct for passing build buffer info via RPC (used by host and device)
typedef struct {
    flexio_uintptr_t addr;
    size_t size;
} build_buffer_info;

#endif // not __CLIENT__
#endif // not __MULTICLIENT_CLIENT__


// --- Bulk Load Configuration ---
// Central configuration for bulk load key range
#define DPASTORE_BULK_LOAD_NUM_KEYS (1000 * 1000 * 25)  // Number of keys to bulk load
#define DPASTORE_BULK_LOAD_MIN_KEY 1ULL      // Starting key value
#define DPASTORE_BULK_LOAD_MAX_KEY (1000ULL * 1000 * 1000 * 10)  // Ending key value (inclusive)

// --- Operation Types ---
// Client Operations
#define DPASTORE_OP_GET    0
#define DPASTORE_OP_INSERT 1
#define DPASTORE_OP_PING   2
#define DPASTORE_OP_RANGE  3
// Internal Operations for Strict Ownership Propagation
#define DPASTORE_OP_INTERNAL_REPLACE_CHILD_SINGLE 4
#define DPASTORE_OP_INTERNAL_REPLACE_CHILD_SPLIT  5
#define DPASTORE_OP_INTERNAL_OP_COMPLETE          6

// --- Status Codes ---
#define DPASTORE_STATUS_OK        0
#define DPASTORE_STATUS_NOTFOUND  1
#define DPASTORE_STATUS_ERROR     2 // Generic error / retry needed
#define DPASTORE_STATUS_RETRY     3 // Explicit retry signal (optional)
#define DPASTORE_STATUS_RETRY_FROM_ROOT 4 // Retry operation from tree root


// Maximum number of KV pairs returned for a range lookup
#define DPASTORE_MAX_RANGE_RESULTS 10

typedef struct {
    uint64_t key;
    uint64_t value;
} __attribute__((packed)) DPAStoreRangeEntry;


// =============================================================================
// CLIENT PACKET - Includes HotCachePlan metadata (25 bytes payload before timestamp)
// Total packet size: 14 (Ethernet) + 20 (IP) + 8 (UDP) + 25 (payload) + 8 (timestamp) = 75 bytes
// =============================================================================
typedef struct {
    // --- Control byte with bit fields (1 byte) ---
    uint8_t  control;               // Bits: op_type(3), status(2), hop_count(3)
                                    // op_type: bits 5-7 (0-7 values)
                                    // status:  bits 3-4 (0-3 values)
                                    // hop_count: bits 0-2 (0-7 max hops)

    // --- Reserved byte for future flags (1 byte) ---
#if DPASTORE_RANGE_REQUEST
    uint8_t  reserved[3];
#else
    uint8_t  reserved[1];
#endif

    // --- Request tracking (4 bytes) ---
    uint32_t request_id;            // Client request ID (4B requests is plenty)

    // --- Core Operation Data (16 bytes) ---
    uint64_t key;                   // Key for operation
    uint64_t value;                 // Value (INSERT) or result (GET)

    HotCachePlan hot_cache_plan;    // Precomputed plan for NIC hot cache

    uint64_t timestamp;

#if DPASTORE_RANGE_REQUEST
    DPAStoreRangeEntry ranges[DPASTORE_MAX_RANGE_RESULTS];
#endif

} __attribute__((packed)) DPAStoreClientPacket;

// Helper macros for control byte manipulation
#define PACKET_SET_OP_TYPE(ctrl, op)     ((ctrl) = ((ctrl) & 0x1F) | (((op) & 0x07) << 5))
#define PACKET_GET_OP_TYPE(ctrl)         (((ctrl) >> 5) & 0x07)

#define PACKET_SET_STATUS(ctrl, stat)    ((ctrl) = ((ctrl) & 0xE7) | (((stat) & 0x03) << 3))
#define PACKET_GET_STATUS(ctrl)          (((ctrl) >> 3) & 0x03)

#define PACKET_SET_HOP_COUNT(ctrl, hop)  ((ctrl) = ((ctrl) & 0xF8) | ((hop) & 0x07))
#define PACKET_GET_HOP_COUNT(ctrl)       ((ctrl) & 0x07)

// Initialize control byte with all fields
#define PACKET_INIT_CONTROL(op, stat, hop) ((((op) & 0x07) << 5) | (((stat) & 0x03) << 3) | ((hop) & 0x07))

// For device builds, the internal packet will be defined in device-specific headers
// For client/host builds, use the simple client packet
#ifndef __DEVICE__
typedef DPAStoreClientPacket DPAStorePacket;
#endif

// Response can potentially reuse the same structure by setting op_type and internal_status
// Or keep a separate simpler one if preferred:
// This simple response struct is only used by the device currently, but could be useful elsewhere.
// Let's keep it here for now, but it doesn't need conditional compilation.
typedef struct {
    uint8_t  status;       // OK, NOT_FOUND, ERROR
    uint8_t  reserved[7];
    uint64_t value;        // Result for GET, echo for INSERT
} __attribute__((packed)) DPAStorePacketRespSimple; // Example if separate needed

#endif // DPASTORE_COMMON_H
