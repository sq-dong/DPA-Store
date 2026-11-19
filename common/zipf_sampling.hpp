#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <random>
#include <utility>
#include <vector>

#include "dpa_common.h"
#include "workload_config.hpp"
#include "partition_layout.hpp"

namespace dpastore {

inline uint64_t xorshift64(uint64_t &state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    if (state == 0) {
        state = 0x9e3779b97f4a7c15ULL;
    }
    return state;
}

inline uint16_t encode_alpha_q16(double alpha) {
    double scaled = alpha * 65536.0;
    if (scaled < 0.0) {
        scaled = 0.0;
    }
    if (scaled > static_cast<double>(std::numeric_limits<uint16_t>::max())) {
        scaled = static_cast<double>(std::numeric_limits<uint16_t>::max());
    }
    return static_cast<uint16_t>(std::lround(scaled));
}

inline double clamp_quantile(double u) {
    const double min_q = std::numeric_limits<double>::min();
    const double max_q = std::nextafter(1.0, 0.0);
    if (u < min_q) {
        return min_q;
    }
    if (u > max_q) {
        return max_q;
    }
    return u;
}

template <typename OutputIt>
void generate_zipf_ranks(double alpha, uint64_t catalog_size, OutputIt begin, OutputIt end)
{
    const size_t count = static_cast<size_t>(std::distance(begin, end));
    if (count == 0 || catalog_size == 0) {
        return;
    }

    std::vector<double> cdf(catalog_size, 0.0);
    double normalizer = 0.0;
    for (uint64_t i = 0; i < catalog_size; ++i) {
        double term = 1.0 / std::pow(static_cast<double>(i + 1ULL), alpha);
        normalizer += term;
        cdf[i] = normalizer;
    }
    if (normalizer == 0.0) {
        std::fill(begin, end, 0U);
        return;
    }

    auto it = begin;
    uint64_t rank_index = 0;
    double cumulative = cdf[0] / normalizer;
    for (size_t i = 0; i < count; ++i, ++it) {
        double target = (static_cast<double>(i) + 0.5) / static_cast<double>(count);
        target = clamp_quantile(target);
        while (rank_index + 1ULL < catalog_size && cumulative < target) {
            ++rank_index;
            cumulative = cdf[rank_index] / normalizer;
        }
        if (rank_index >= catalog_size) {
            rank_index = catalog_size - 1ULL;
        }
        *it = static_cast<uint32_t>(rank_index);
    }
}

inline std::vector<uint32_t> make_zipf_table(const ZipfWorkloadConfig &cfg,
                                             uint64_t catalog_size,
                                             uint32_t table_entries)
{
    std::vector<uint32_t> table(table_entries, 0U);
    generate_zipf_ranks(cfg.alpha, catalog_size, table.begin(), table.end());
    return table;
}

inline uint64_t draw_random_value(std::mt19937_64 &rng) {
    return rng();
}

} // namespace dpastore
