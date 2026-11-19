#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>


#define RANGE_QUERIES_ENABLED       0
#define MAX_RANGE_QUERIES           10

#define PACKET_INIT_CONTROL(op, stat, hop) ((((op) & 0x07) << 5) | (((stat) & 0x03) << 3) | ((hop) & 0x07))

enum {
    HOT_CACHE_MAX_ENTRIES = 96, /* Effective admission budget */
    HOT_CACHE_SET_COUNT = 64,  /* Physical table has 256 sets */
    HOT_CACHE_SET_MASK = HOT_CACHE_SET_COUNT - 1,
    HOT_CACHE_WAYS = 4,         /* Four ways per set */
    HOT_CACHE_TABLE_CAPACITY = HOT_CACHE_SET_COUNT * HOT_CACHE_WAYS,
    HOT_CACHE_SET_BYTES = 64,
    HOT_CACHE_EMPTY_KEY = UINT64_MAX
};

/* 32-byte Bloom filter: four 64-bit words already reside in a fetched line. */
enum {
    HOT_CACHE_BLOOM_WORDS = 4,
    HOT_CACHE_BLOOM_BITS = HOT_CACHE_BLOOM_WORDS * 64,
    HOT_CACHE_BLOOM_K = 3, /* double hashing yields 3 positions per key */
    HOT_CACHE_BLOOM_REBUILD_INTERVAL = 2048 /* accesses between rebuilds */
};

typedef struct {
    uint64_t words[HOT_CACHE_BLOOM_WORDS];
} HotCacheBloom;

/* Space-Saving detector keeps the heaviest ~8 fingerprints with 8-bit counters. */
enum {
    HOTNESS_TRACKER_SLOTS = 8,
    HOTNESS_PROMOTE_THRESHOLD = 3,
    HOTNESS_DECAY_INTERVAL = 4096,
    HOTNESS_SAMPLE_MASK = 0x7 /* sample every 8th access on average */
};

typedef struct {
    uint16_t fingerprint;
    uint8_t counter;
} HotnessCandidate;

typedef struct {
    HotnessCandidate slots[HOTNESS_TRACKER_SLOTS];
    uint16_t decay_budget; /* decremented with each sampled update */
} HotnessTracker;

/*
 * Client-side access plan (HotCachePlan)
 * --------------------------------------
 * Client threads call hot_cache_plan_init_from_key/hash and stash the result in
 * the request packet. The fields line up with everything the server needs:
 *   bloom_bits : HOT_CACHE_BLOOM_K entries, each a bit index (0..255). The
 *                first entry doubles as the bucket index for the hash map.
 *
 * Server traversers consume the plan directly to keep their hot path free of
 * mixing math or extra hashing.
 */
typedef struct {
    uint8_t bloom_bits[HOT_CACHE_BLOOM_K];
} HotCachePlan __attribute__((packed));

// typedef struct {
//     uint64_t key_hash;
//     uint16_t fingerprint;
//     uint8_t bloom_bits[HOT_CACHE_BLOOM_K];
//     uint8_t padding[3];
// } HotCachePlan __attribute__((packed));;

typedef struct {
    uint64_t key;
    uint64_t value;
} HotCacheEntry;

typedef struct {
    HotCacheEntry ways[HOT_CACHE_WAYS];
    uint8_t padding[HOT_CACHE_SET_BYTES - HOT_CACHE_WAYS * sizeof(HotCacheEntry)];
} HotCacheSet;

typedef struct {
    uint32_t victim_rng; /* LCG state for random victim selection */
    HotCacheSet sets[HOT_CACHE_SET_COUNT];
} HotCacheTable;

typedef struct {
    HotCacheBloom bloom;
    HotCacheTable table;
    uint32_t rebuild_countdown;
#if HOT_CACHE_USE_PROBABILITY_TRACKING
    uint32_t bloom_saturation;
#endif
} HotCache;

#ifndef HOT_CACHE_USE_PROBABILITY_TRACKING
#define HOT_CACHE_USE_PROBABILITY_TRACKING 1
#endif

#ifndef HOT_CACHE_ADMIT_PROB_Q16
#define HOT_CACHE_ADMIT_PROB_Q16 8192 /* ~12.5% admission probability */
#endif

#ifndef HOT_CACHE_PROB_MAX_ADMITS
#define HOT_CACHE_PROB_MAX_ADMITS 96
#endif

#if defined(__cplusplus)
#define HOT_CACHE_STATIC_ASSERT(expr, msg) static_assert((expr), msg)
#else
#define HOT_CACHE_STATIC_ASSERT(expr, msg) _Static_assert((expr), msg)
#endif

HOT_CACHE_STATIC_ASSERT((HOT_CACHE_SET_COUNT & HOT_CACHE_SET_MASK) == 0,
                        "Set count must be a power of two");
HOT_CACHE_STATIC_ASSERT(HOT_CACHE_TABLE_CAPACITY == HOT_CACHE_SET_COUNT * HOT_CACHE_WAYS,
                        "Table capacity mismatch");
HOT_CACHE_STATIC_ASSERT(HOT_CACHE_MAX_ENTRIES <= HOT_CACHE_TABLE_CAPACITY,
                        "Admission budget exceeds table capacity");
HOT_CACHE_STATIC_ASSERT(HOT_CACHE_WAYS == 4,
                        "Current layout assumes 4 ways per set");
HOT_CACHE_STATIC_ASSERT(sizeof(HotCacheBloom) == 32,
                        "Bloom filter must stay inside 32-byte budget");
HOT_CACHE_STATIC_ASSERT(HOT_CACHE_BLOOM_BITS <= 256,
                        "Bloom plan stores bit positions in uint8_t");
// HOT_CACHE_STATIC_ASSERT(sizeof(HotCachePlan) == HOT_CACHE_BLOOM_K,
//                         "HotCachePlan only carries Bloom bit indices");
HOT_CACHE_STATIC_ASSERT(HOT_CACHE_WAYS * sizeof(HotCacheEntry) <= HOT_CACHE_SET_BYTES,
                        "Hot cache set must fit within one cache line");
HOT_CACHE_STATIC_ASSERT(sizeof(HotCacheSet) == HOT_CACHE_SET_BYTES,
                        "Hot cache set is padded to a single cache line");



namespace protocol {

constexpr std::size_t kRequestWordCount = 4;      // op, key, value/length, request_id
constexpr std::size_t kResponseBaseWordCount = 4; // status, key, value/length, request_id

#define PROTOCOL_STATUS_OK          0
#define PROTOCOL_STATUS_NOT_FOUND   1
#define PROTOCOL_STATUS_ERROR       2
#define PROTOCOL_STATUS_BAD_REQUEST 3

#define PROTOCOL_OP_GET             0
#define PROTOCOL_OP_INSERT          1
#define PROTOCOL_OP_PING            2
#define PROTOCOL_OP_RANGE           3
// TODO @fred: check API with DPAStore
#define PROTOCOL_OP_DELETE          4
#define PROTOCOL_NUM_OP_TYPES       5
#define PROTOCOL_OP_UNKNOWN         7

/*
 * Ethernet = 14B
 * IPv4     = 20B
 * UPD      =  8B
 * Sum      = 42B
 *
 * With non-range query: 42B + 48B        =  90B < 128B
 * With range query:     42B + 48B + 160B = 250B < 256B
 */
struct ControlBlock {
    uint8_t op:3;
    uint8_t status:2;
    uint8_t hop_count:3; // TODO ...what?
} __attribute__((packed));

#if RANGE_QUERIES_ENABLED
struct KVPair {
    std::uint64_t key;
    std::uint64_t value;
} __attribute__((packed));
#endif

struct KVPacket {
    uint8_t control;
    std::uint8_t padding[1];
    union {
        std::uint32_t id;
        struct {
            std::uint16_t queue_id;
            std::uint16_t slot_id;
        };
    };
    std::uint64_t key{0};
    union {
        std::uint64_t value{0};
        std::uint64_t len;
    };
    HotCachePlan hot_cache_plan;
    std::uint64_t timestamp;
#if RANGE_QUERIES_ENABLED
    struct KVPair range[MAX_RANGE_QUERIES];
#endif
} __attribute__((packed));

struct DecodeError {
    const char* message;
};

struct EncodeError {
    const char* message;
};

// Map key → server reader UDP port in [base, base+num_readers).
// num_readers must match the server's DPASTORE_READER_THREAD_COUNT.
inline std::uint16_t dst_port(std::uint16_t base_port,
                              std::uint16_t num_readers, std::uint64_t key) {
    if (num_readers <= 1) {
        return base_port;
    }
    // High-multiply maps the full 64-bit key space uniformly into [0, num_readers).
    const __uint128_t scaled =
        static_cast<__uint128_t>(key) * static_cast<__uint128_t>(num_readers);
    const std::uint64_t bucket = static_cast<std::uint64_t>(scaled >> 64);
    return base_port + static_cast<std::uint16_t>(bucket);
}

inline bool op_valid(const uint8_t op) {
    return op < PROTOCOL_NUM_OP_TYPES;
}

}  // namespace protocol
