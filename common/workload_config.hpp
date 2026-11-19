#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "dpastore_common.h"

namespace dpastore {

constexpr uint32_t kZipfTableDefaultEntries = 1U << 17;

// Compile-time overrides (pass via -D on the build command line)
#ifndef DPASTORE_ZIPF_ALPHA
#define DPASTORE_ZIPF_ALPHA 1.1
#endif

#ifndef DPASTORE_ZIPF_SEED
#define DPASTORE_ZIPF_SEED 0x9e3779b97f4a7c15ULL
#endif

#ifndef DPASTORE_ZIPF_TABLE_ENTRIES
// 0 means "auto": we size the shared Zipf table to min(keys_per_partition, max_table) at runtime.
#define DPASTORE_ZIPF_TABLE_ENTRIES 0U
#endif

#ifndef DPASTORE_ZIPF_TOTAL_KEYS
#define DPASTORE_ZIPF_TOTAL_KEYS DPASTORE_BULK_LOAD_NUM_KEYS
#endif

struct ZipfWorkloadConfig {
    double alpha;
    uint64_t seed;
    uint32_t table_size;   // entries in the shared Zipf table
    uint64_t total_keys;   // total keys to bulk load (host) or target catalog size
    uint64_t min_key;
    uint64_t max_key;
};

inline ZipfWorkloadConfig make_default_zipf_config() {
    ZipfWorkloadConfig cfg{};
    cfg.alpha = static_cast<double>(DPASTORE_ZIPF_ALPHA);
    cfg.seed = static_cast<uint64_t>(DPASTORE_ZIPF_SEED);
    cfg.table_size = static_cast<uint32_t>(DPASTORE_ZIPF_TABLE_ENTRIES);
    cfg.total_keys = static_cast<uint64_t>(DPASTORE_ZIPF_TOTAL_KEYS);
    cfg.min_key = DPASTORE_BULK_LOAD_MIN_KEY;
    cfg.max_key = DPASTORE_BULK_LOAD_MAX_KEY;
    return cfg;
}

inline ZipfWorkloadConfig load_zipf_config_from_defines() {
    return make_default_zipf_config();
}

inline std::string describe(const ZipfWorkloadConfig &cfg) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "alpha=%.4f seed=%#018llx table=%u total_keys=%llu range=[%llu,%llu]",
                  cfg.alpha,
                  static_cast<unsigned long long>(cfg.seed),
                  static_cast<unsigned int>(cfg.table_size),
                  static_cast<unsigned long long>(cfg.total_keys),
                  static_cast<unsigned long long>(cfg.min_key),
                  static_cast<unsigned long long>(cfg.max_key));
    return std::string(buffer);
}

} // namespace dpastore
