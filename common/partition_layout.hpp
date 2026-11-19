#pragma once

#include <cstdint>

#include "dpa_common.h"
#include "dpastore_common.h"
#include "workload_config.hpp"

namespace dpastore {

inline uint64_t compute_partition_span(const ZipfWorkloadConfig &cfg) {
    const uint64_t range = (cfg.max_key - cfg.min_key) + 1ULL;
    uint64_t span = range / DPASTORE_READER_THREAD_COUNT;
    if (range % DPASTORE_READER_THREAD_COUNT != 0ULL) {
        ++span;
    }
    return span == 0ULL ? 1ULL : span;
}

inline uint64_t compute_partition_target(const ZipfWorkloadConfig &cfg) {
    const uint64_t total = cfg.total_keys;
    uint64_t per = total / DPASTORE_READER_THREAD_COUNT;
    if (total % DPASTORE_READER_THREAD_COUNT != 0ULL) {
        ++per;
    }
    return per == 0ULL ? 1ULL : per;
}

inline uint64_t partition_base_key(const ZipfWorkloadConfig &cfg, uint32_t thread_id, uint64_t partition_span) {
    uint64_t base = cfg.min_key + partition_span * static_cast<uint64_t>(thread_id);
    if (base > cfg.max_key) {
        base = cfg.max_key;
    }
    return base;
}

inline uint64_t partition_limit_key(const ZipfWorkloadConfig &cfg, uint32_t thread_id, uint64_t partition_span) {
    uint64_t base = partition_base_key(cfg, thread_id, partition_span);
    uint64_t limit = base + partition_span;
    uint64_t max_plus_one = cfg.max_key + 1ULL;
    if (limit > max_plus_one) {
        limit = max_plus_one;
    }
    if (limit <= base) {
        limit = base + 1ULL;
    }
    return limit;
}

inline uint64_t mix_partition_seed(uint64_t base_seed, uint32_t thread_id) {
    uint64_t mixed = base_seed ^ (0x9e3779b97f4a7c15ULL * (static_cast<uint64_t>(thread_id) + 1ULL));
    if (mixed == 0ULL) {
        mixed = 0x5851f42d4c957f2dULL ^ static_cast<uint64_t>(thread_id + 1ULL);
    }
    return mixed;
}

} // namespace dpastore

