#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "protocol.hpp"

// --------------------------------------
// PUBLIC TYPES
// --------------------------------------
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

enum class OperationType : std::uint64_t {
    Get = 0,
    Insert = 1,
    Ping = 2,
    Delete = 3,
    Range = 4,
};

struct Operation {
    std::uint32_t id{};
    OperationType type{OperationType::Get};
    std::uint64_t key{};
    union {
        std::uint64_t value{};
        std::uint64_t len;
    };

    Operation() {}

    Operation(const size_t &id, const OperationType &type,
              const std::uint64_t &key, const std::uint64_t &val)
        : id(id), type(type), key(key), value(val) {}

    const std::string to_string() const {
        if (type == OperationType::Get) {
            return "{ id = " + std::to_string(id) + ", type = GET, key = " + std::to_string(key) + " }";
        } else if (type == OperationType::Insert) {
            return "{ id = " + std::to_string(id) + ", type = PUT, key = " + std::to_string(key) + ", val = " + std::to_string(value) + " }";
        } else if (type == OperationType::Ping) {
            return "{ id = " + std::to_string(id) + ", type = PING }";
        } else if (type == OperationType::Delete) {
            return "{ id = " + std::to_string(id) + ", type = DELETE, key = " + std::to_string(key) + " }";
        } else {
            return "{ id = " + std::to_string(id) + ", type = RANGE, key = " + std::to_string(key) + ", len = " + std::to_string(len) + " }";
        }
    }

    void to_packet(protocol::KVPacket *p) const {
        p->id = id;
        p->key = key;

        hot_cache_plan_init_from_key(&p->hot_cache_plan, key);

        if (type == OperationType::Get) {
            p->control = PACKET_INIT_CONTROL(PROTOCOL_OP_GET, 0, 0);
        } else if (type == OperationType::Insert) {
            p->control = PACKET_INIT_CONTROL(PROTOCOL_OP_INSERT, 0, 0);
            p->value = value;
        } else if (type == OperationType::Delete) {
            p->control = PACKET_INIT_CONTROL(PROTOCOL_OP_GET, 0, 0);
        } else if (type == OperationType::Range) {
            p->control = PACKET_INIT_CONTROL(PROTOCOL_OP_RANGE, 0, 0);
            p->len = len;
        } else if (type == OperationType::Ping) {
            p->control = PACKET_INIT_CONTROL(PROTOCOL_OP_PING, 0, 0);
        } else {
            p->control = PACKET_INIT_CONTROL(PROTOCOL_OP_PING, 0, 0);
        }

    }
};

enum class OperationStatus : std::uint8_t {
    Finished,
    Pending,
};

struct OperationResult {
    OperationStatus status{OperationStatus::Pending};
    std::uint64_t latency_ns{0};
};

// --------------------------------------
// PUBLIC FUNCTIONS
// --------------------------------------

// Generates a new operation for the benchmark.
Operation get_next_operation();


// --------------------------------------
// INTERFACE TO BE IMPLEMENTED
// --------------------------------------

int target_on_benchmark_ready(int argc, char **argv);

#if CONCURRENCY_MODE == FULL_CUSTOM
void target_on_benchmark_run();
#endif // CONCURRENCY_MODE == FULL_CUSTOM

// Target-specific hooks implemented by the selected key-value store benchmark backend.
void target_thread_init(std::size_t thread_id);
void target_thread_exit(std::size_t thread_id);

#if CONCURRENCY_MODE == CUSTOM_THREADED
// Optional custom threading entry point for CUSTOM_THREADED mode.
void target_thread_execute(std::size_t thread_id);
#endif

// Called by the backend to issue new operations in DEFAULT_THREADED mode.
OperationResult target_do_operation(std::size_t thread_id,
                                    std::size_t bucket_id,
                                    const Operation& operation);

// Called by the backend to query which buckets completed pending operations.
std::vector<std::size_t> target_check_for_replies(std::size_t thread_id);
