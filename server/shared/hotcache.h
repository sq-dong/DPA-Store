#ifndef DPA_STORE_HOTCACHE_H
#define DPA_STORE_HOTCACHE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Hot Cache Overview
 * ------------------
 * Per-thread cache sized for the DPA traverser: a 256-set × 4-way table (1,024
 * physical slots) padded so each lookup stays within a single 64-byte line. We
 * only admit ~96 entries per thread to keep the 32-byte Bloom filter useful;
 * once that budget is exhausted we stop inserting new keys. Clients precompute a
 * HotCachePlan (just the Bloom bit positions) and ship it with the packet so
 * the device-side lookup reduces to a Bloom test and one set probe. The Bloom
 * filter consumes 32 bytes (four 64-bit words, k=3). Because we cap admissions,
 * it keeps enough signal to short-circuit most cold probes.
 *
 * Detailed notes below capture Bloom sizing, workload thresholds, and layout
 * sketches.
 * -----------------------------------
 * Goal
 *   Build a per-thread hot-key cache that fits DPA constraints: integer-only
 *   math, minimal state (<2 KB), and deterministic latency. The cache must
 *   spot Zipfian hot spots quickly, avoid expensive lookups when the key is
 *   cold, and keep metadata compact enough to live inside already fetched cache
 *   lines on the NIC. The dominant budget limiter is the per-thread Bloom
 *   filter: only 32 bytes (4 x 8-byte words) are available, so every other
 *   structure is organized so that a lookup or update stays within a single
 *   cache-line access.
 *
 * Architecture
 *   - Tiny Bloom filter (32 B = 256 bits) lives in spare space inside the same
 *     cache line as other metadata. It answers "might be cached?" so traversers
 *     can skip hash-table probes for most cold keys. A rebuild every so often
 *     keeps false positives bounded despite the very tight footprint.
 *   - Client computes a HotCachePlan per request (Bloom bit positions derived
 *     from the key hash) and ships it alongside the key so server traversers
 *     perform only table lookups and bit tests.
 *   - Hash map capacity is flexible, but each probe must touch only one cache
 *     line. We use power-of-two set indexing where every 64-byte set contains
 *     four 16-byte entries, so the caller issues exactly one line read per
 *     lookup regardless of total table size.
 *
 * Bloom sizing and workload guidance
 * ---------------------------------
 * Bloom filter (32 B = 256 bits, HOT_CACHE_BLOOM_K = 3):
 *   False-positive rate p ≈ (1 - e^{-(k·n)/m})^k with m = 256 bits, k = 3
 *   Budgeting keeps the effective n near the bloom capacity so the filter retains
 *   useful signal.
 *
 * Zipf workload thresholds (per thread):
 *   - 188 threads own disjoint key ranges → each sees the same Zipf exponent α
 *   - Per-thread catalog size N_thread = N_total / 188. For common totals:
 *
 *     N_total            N_thread      top-96 hit share (%)
 *                       (distinct)   α=0.8  α=0.9  α=1.0  α=1.1  α=1.2  α=1.3
 *     ------------------------------------------------------------------------
 *     1,000,000            5,319      34.4   44.9   56.2   67.0   76.3   83.7
 *    10,000,000           53,191      20.3   31.4   44.9   58.9   71.3   80.9
 *    15,000,000           79,787      18.5   29.6   43.4   57.8   70.6   80.6
 *   200,000,000        1,063,829      10.6   20.8   35.6   52.5   67.8   79.3
 * 1,000,000,000        5,319,148       7.6   16.9   32.0   50.2   66.7   78.9
 *
 *   Interpretation: with a 32 B Bloom (≈15% FPR when n=96), you need α ≳ 1.0
 *   once the per-thread catalog exceeds ~50k keys. Steeper skews (α ≥ 1.1)
 *   deliver 50%+ cache hit rates even with millions of keys per thread; flatter
 *   skews (α ≤ 0.9) fall below the Bloom noise and provide little benefit.
 */

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

/* ---------------------------- Utility hashing ---------------------------- */
static inline uint64_t hot_cache_mix64(uint64_t key)
{
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return key;
}

static inline uint64_t hot_cache_hash_key(uint64_t key)
{
    return hot_cache_mix64(key);
}

/* ----------------------------- Bloom filter ------------------------------ */
/*
 * Bloom Filter Layout (32 B = 4 x 64-bit words)
 * ---------------------------------------------
 *   word[3] ............ word[1] | word[0]
 *   ---------------------------------------
 *  |63 ... 0| ......... |63 ... 0|63 ... 0|
 *   ---------------------------------------
 *              ^        ^        ^
 *              |------ plan->bloom_bits[] select absolute bit positions
 *
 * Each plan supplies HOT_CACHE_BLOOM_K precomputed bit indices; the server only
 * touches the addressed words, keeping lookups branch-light.
 */
static inline void hot_cache_bloom_clear(HotCacheBloom *bloom)
{
    for (uint32_t i = 0; i < HOT_CACHE_BLOOM_WORDS; ++i) {
        bloom->words[i] = 0ULL;
    }
}

static inline uint32_t hot_cache_bloom_map(uint64_t hash)
{
    const uint32_t hash32 = (uint32_t)(hash ^ (hash >> 32));
    const uint64_t product = (uint64_t)hash32 * HOT_CACHE_BLOOM_BITS;
    return (uint32_t)(product >> 32); /* maps 32-bit hash into 0..(bits-1) */
}

static inline uint32_t hot_cache_bloom_bit(uint64_t base, uint64_t step, uint32_t round)
{
    const uint64_t combined = base + step * round;
    return hot_cache_bloom_map(combined);
}

static inline void hot_cache_plan_init_from_hash(HotCachePlan *plan, uint64_t key_hash)
{
    const uint64_t step = hot_cache_mix64(key_hash + 0x9e3779b97f4a7c15ULL);
    for (uint32_t i = 0; i < HOT_CACHE_BLOOM_K; ++i) {
        plan->bloom_bits[i] = (uint8_t)hot_cache_bloom_bit(key_hash, step, i);
    }
}

static inline void hot_cache_plan_init_from_key(HotCachePlan *plan, uint64_t key)
{
    uint64_t mix = key;
    mix ^= mix >> 33;
    mix *= 0xff51afd7ed558ccdULL;
    mix ^= mix >> 33;
    mix *= 0xc4ceb9fe1a85ec53ULL;
    mix ^= mix >> 33;
    hot_cache_plan_init_from_hash(plan, mix);
}

static inline bool hot_cache_plan_should_admit(const HotCachePlan *plan)
{
    (void)plan;
    return true;
}

static inline void hot_cache_bloom_add_plan(HotCacheBloom *bloom, const HotCachePlan *plan)
{
    for (uint32_t i = 0; i < HOT_CACHE_BLOOM_K; ++i) {
        const uint32_t bit = plan->bloom_bits[i];
        const uint32_t word = bit >> 6;
        const uint32_t offset = bit & 63U;
        bloom->words[word] |= (1ULL << offset);
    }
}

static inline bool hot_cache_bloom_has_plan(const HotCacheBloom *bloom, const HotCachePlan *plan)
{
    for (uint32_t i = 0; i < HOT_CACHE_BLOOM_K; ++i) {
        const uint32_t bit = plan->bloom_bits[i];
        const uint32_t word = bit >> 6;
        const uint32_t offset = bit & 63U;

        if ((bloom->words[word] & (1ULL << offset)) == 0ULL) {
            return false;
        }
    }

    return true;
}

static inline void hot_cache_bloom_rebuild(HotCacheBloom *bloom, const HotCacheTable *table)
{
    hot_cache_bloom_clear(bloom);

    HotCachePlan plan;
    for (uint32_t set_idx = 0; set_idx < HOT_CACHE_SET_COUNT; ++set_idx) {
        const HotCacheSet *set = &table->sets[set_idx];
        for (uint32_t way = 0; way < HOT_CACHE_WAYS; ++way) {
            if (set->ways[way].key != HOT_CACHE_EMPTY_KEY) {
                hot_cache_plan_init_from_key(&plan, set->ways[way].key);
                hot_cache_bloom_add_plan(bloom, &plan);
            }
        }
    }
}

/* ------------------------------ Hash table ------------------------------- */
/*
 * Hash table layout (single cache line per set)
 * ---------------------------------------------
 * set_idx = plan->bloom_bits[0] & HOT_CACHE_SET_MASK
 *
 * 64-byte set structure:
 *   -------------------------------------------------
 *  | way0 (key,value) | way1 (key,value) | way2 | way3 |
 *   -------------------------------------------------
 *              ^                 ^
 *              |------ traverser probes at most HOT_CACHE_WAYS entries
 *
 * Random victim selection uses a multiply-high trick to avoid division while
 * keeping distribution uniform across HOT_CACHE_WAYS ways.
 */
static inline void hot_cache_table_init(HotCacheTable *table)
{
    for (uint32_t set_idx = 0; set_idx < HOT_CACHE_SET_COUNT; ++set_idx) {
        for (uint32_t way = 0; way < HOT_CACHE_WAYS; ++way) {
            table->sets[set_idx].ways[way].key = HOT_CACHE_EMPTY_KEY;
            table->sets[set_idx].ways[way].value = 0ULL;
        }
    }
    table->victim_rng = 1U;
}

static inline uint32_t hot_cache_table_set_index(const HotCachePlan *plan)
{
    return (uint32_t)plan->bloom_bits[0] & HOT_CACHE_SET_MASK;
}

static inline uint32_t hot_cache_table_pick_victim(HotCacheTable *table)
{
    /* LCG with odd increment keeps a short period but is cheap on hardware. */
    table->victim_rng = table->victim_rng * 1664525U + 1013904223U;
    return (uint32_t)(((uint64_t)table->victim_rng * HOT_CACHE_WAYS) >> 32);
}

static inline bool hot_cache_table_lookup_plan(const HotCacheTable *table, uint64_t key,
                                               const HotCachePlan *plan, uint64_t *value_out)
{
    const uint32_t set_idx = hot_cache_table_set_index(plan);
    const HotCacheSet *set = &table->sets[set_idx];

    for (uint32_t way = 0; way < HOT_CACHE_WAYS; ++way) {
        if (set->ways[way].key == key) {
            if (value_out != NULL) {
                *value_out = set->ways[way].value;
            }
            return true;
        }
    }

    return false;
}

static inline void hot_cache_table_insert_plan(HotCacheTable *table, uint64_t key,
                                               const HotCachePlan *plan, uint64_t value)
{
    const uint32_t set_idx = hot_cache_table_set_index(plan);
    HotCacheSet *set = &table->sets[set_idx];

    for (uint32_t way = 0; way < HOT_CACHE_WAYS; ++way) {
        if (set->ways[way].key == key) {
            set->ways[way].value = value;
            return;
        }
    }

    for (uint32_t way = 0; way < HOT_CACHE_WAYS; ++way) {
        if (set->ways[way].key == HOT_CACHE_EMPTY_KEY) {
            set->ways[way].key = key;
            set->ways[way].value = value;
            return;
        }
    }

    const uint32_t victim = hot_cache_table_pick_victim(table);
    set->ways[victim].key = key;
    set->ways[victim].value = value;
}

#undef HOT_CACHE_STATIC_ASSERT

#endif /* DPA_STORE_HOTCACHE_H */
