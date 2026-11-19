#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * UniformPlan keeps a deterministic mapping from a logical rank index [0, count)
 * inside a partition to an actual key offset within that partition's span.
 * The mapping introduces a small per-rank jitter so successive keys are spaced
 * quasi-uniformly without collapsing into perfectly sequential values. The same
 * plan is used on the host (bulk load) and on the device (throughput client) so
 * both sides agree on the key catalog without materialising it.
 */
typedef struct {
    uint64_t base;         /* Partition base key (inclusive) */
    uint64_t span;         /* Partition span in keys (exclusive) */
    uint64_t count;        /* Number of keys assigned to the partition */
    uint64_t seed;         /* Mixed seed per partition */
    uint64_t stride;       /* Co-prime stride used for permutation */
    uint64_t stride_inv;   /* Modular inverse of stride modulo span */
    uint64_t offset_base;  /* Rotation offset inside span */
} UniformPlan;

#define UNIFORM_JITTER_BITS 8U
#define UNIFORM_JITTER_SCALE (1U << UNIFORM_JITTER_BITS)

static inline uint64_t uniform_mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static inline uint8_t uniform_jitter_sample(uint64_t seed, uint64_t rank)
{
    uint64_t mixed = uniform_mix64(seed ^ (0x9e3779b97f4a7c15ULL * (rank + 1ULL)));
    return (uint8_t)(mixed & 0xFFU);
}

static inline uint64_t uniform_gcd(uint64_t a, uint64_t b)
{
    while (b != 0ULL) {
        uint64_t tmp = b;
        b = a % b;
        a = tmp;
    }
    return a;
}

static inline uint64_t uniform_mul_mod(uint64_t a, uint64_t b, uint64_t mod)
{
#if defined(__SIZEOF_INT128__)
    __uint128_t prod = ( __uint128_t )a * ( __uint128_t )b;
    return (uint64_t)(prod % mod);
#else
    uint64_t result = 0ULL;
    while (b > 0ULL) {
        if (b & 1ULL) {
            result = (result + a) % mod;
        }
        a = (a * 2ULL) % mod;
        b >>= 1ULL;
    }
    return result;
#endif
}

static inline uint64_t uniform_mod_inverse(uint64_t a, uint64_t mod)
{
    int64_t t = 0, new_t = 1;
    int64_t r = (int64_t)mod, new_r = (int64_t)a;

    while (new_r != 0) {
        int64_t q = r / new_r;
        int64_t tmp = new_t;
        new_t = t - q * new_t;
        t = tmp;
        tmp = new_r;
        new_r = r - q * new_r;
        r = tmp;
    }

    if (r > 1) {
        return 1ULL; /* inverse doesn't exist; shouldn't happen */
    }
    if (t < 0) {
        t += (int64_t)mod;
    }
    return (uint64_t)t;
}

static inline uint64_t uniform_pick_stride(uint64_t span, uint64_t seed)
{
    if (span <= 1ULL) {
        return 1ULL;
    }

    uint64_t candidate = (uniform_mix64(seed) | 1ULL) % span;
    if (candidate == 0ULL) {
        candidate = 1ULL;
    }
    while (uniform_gcd(candidate, span) != 1ULL) {
        candidate = (candidate + 2ULL) % span;
        if (candidate == 0ULL) {
            candidate = 1ULL;
        }
    }
    return candidate;
}

static inline void uniform_plan_init(UniformPlan *plan,
                                     uint64_t base,
                                     uint64_t span,
                                     uint64_t count,
                                     uint64_t seed)
{
    if (span == 0ULL) {
        span = 1ULL;
    }
    plan->base = base;
    plan->span = span;
    plan->count = (count == 0ULL) ? 1ULL : count;
    if (plan->count > span) {
        plan->count = span;
    }
    plan->seed = seed;
    plan->stride = uniform_pick_stride(span, seed);
    plan->stride_inv = uniform_mod_inverse(plan->stride % span, span);
    plan->offset_base = uniform_mix64(seed ^ 0xdeadbeefULL) % span;
}

static inline uint64_t uniform_key_offset(const UniformPlan *plan, uint64_t rank)
{
    uint64_t span = plan->span ? plan->span : 1ULL;
    uint64_t count = plan->count ? plan->count : 1ULL;
    if (count > span) {
        count = span;
    }
    uint64_t step = uniform_mul_mod(rank % span, plan->stride, span);
    uint64_t offset = plan->offset_base + step;
    if (offset >= span) {
        offset %= span;
    }
    return offset;
}

static inline uint64_t uniform_key_for_rank(const UniformPlan *plan, uint64_t rank)
{
    return plan->base + uniform_key_offset(plan, rank);
}

static inline uint64_t uniform_rank_from_offset(const UniformPlan *plan, uint64_t offset)
{
    uint64_t span = plan->span ? plan->span : 1ULL;
    if (offset >= span) {
        offset %= span;
    }
    uint64_t adjusted;
    if (offset >= plan->offset_base) {
        adjusted = offset - plan->offset_base;
    } else {
        adjusted = span - (plan->offset_base - offset);
    }
    uint64_t rank = uniform_mul_mod(adjusted, plan->stride_inv, span);
    uint64_t count = plan->count ? plan->count : 1ULL;
    if (rank >= count) {
        rank %= count;
    }
    return rank;
}

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
namespace dpastore {

static inline UniformPlan make_uniform_plan(uint64_t base,
                                            uint64_t span,
                                            uint64_t count,
                                            uint64_t seed)
{
    UniformPlan plan;
    uniform_plan_init(&plan, base, span, count, seed);
    return plan;
}

static inline uint64_t uniform_catalog_key(const UniformPlan &plan, uint64_t rank)
{
    return uniform_key_for_rank(&plan, rank);
}

}
#endif
