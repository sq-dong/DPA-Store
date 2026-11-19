#ifndef TEST_DATA_GENERATOR_HPP
#define TEST_DATA_GENERATOR_HPP

#include <vector>
#include <random>
#include <algorithm>
#include <string>
#include <doca_log.h>

class TestDataGenerator {
public:
    // Generate sequential sorted data
    static std::vector<KVPair> generate_sequential(size_t count, uint64_t start_key = 1) {
        std::vector<KVPair> data;
        data.reserve(count);
        
        for (size_t i = 0; i < count; ++i) {
            data.emplace_back(start_key + i, start_key + i);  // Simple key=value
        }
        
        return data;
    }
    
    // Generate random sorted data with seed for reproducibility
    static std::vector<KVPair> generate_random_sorted(size_t count, 
                                                      uint64_t seed = 42,
                                                      uint64_t min_key = 1, 
                                                      uint64_t max_key = UINT64_MAX / 2) {
        std::vector<KVPair> data;
        data.reserve(count);
        
        std::mt19937_64 gen(seed);  // Use provided seed for reproducibility
        std::uniform_int_distribution<uint64_t> key_dist(min_key, max_key);
        std::uniform_int_distribution<uint64_t> val_dist(0, UINT64_MAX);
        
        // Generate unique keys
        std::vector<uint64_t> keys;
        keys.reserve(count);
        
        while (keys.size() < count) {
            uint64_t key = key_dist(gen);
            keys.push_back(key);
        }
        
        // Sort and remove duplicates
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        
        // Regenerate if we don't have enough unique keys
        while (keys.size() < count) {
            keys.push_back(key_dist(gen));
        }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        
        // Create KV pairs
        for (size_t i = 0; i < std::min(count, keys.size()); ++i) {
            data.emplace_back(keys[i], val_dist(gen));
        }
        
        return data;
    }
    
    // Generate data with gaps (useful for testing splits)
    static std::vector<KVPair> generate_with_gaps(size_t count, uint64_t gap_size = 100) {
        std::vector<KVPair> data;
        data.reserve(count);
        
        uint64_t current_key = 1000;
        for (size_t i = 0; i < count; ++i) {
            data.emplace_back(current_key, current_key * 2);  // Value is 2x key
            current_key += gap_size;
        }
        
        return data;
    }
    
    // Generate clustered data (groups of nearby keys) with seed
    static std::vector<KVPair> generate_clustered(size_t count, 
                                                  size_t num_clusters = 10,
                                                  uint64_t cluster_spread = 1000,
                                                  uint64_t seed = 42) {
        std::vector<KVPair> data;
        data.reserve(count);
        
        std::mt19937_64 gen(seed);  // Use provided seed
        
        size_t items_per_cluster = count / num_clusters;
        uint64_t cluster_base = 1000000;
        
        for (size_t c = 0; c < num_clusters; ++c) {
            uint64_t base = cluster_base + c * 10000000;
            std::uniform_int_distribution<uint64_t> offset_dist(0, cluster_spread);
            
            for (size_t i = 0; i < items_per_cluster; ++i) {
                uint64_t key = base + offset_dist(gen);
                data.emplace_back(key, key);
            }
        }
        
        // Fill remaining items
        while (data.size() < count) {
            data.emplace_back(cluster_base + data.size(), cluster_base + data.size());
        }
        
        // Sort the data
        std::sort(data.begin(), data.end(), 
                  [](const KVPair& a, const KVPair& b) { return a.key < b.key; });
        
        return data;
    }
    
    // Generate worst-case data for tree (all keys hash to same positions)
    static std::vector<KVPair> generate_worst_case(size_t count) {
        std::vector<KVPair> data;
        data.reserve(count);
        
        // Keys that are very close together
        uint64_t base = 1000000000;
        for (size_t i = 0; i < count; ++i) {
            data.emplace_back(base + i, i);
        }
        
        return data;
    }
    
    // Utility to print data statistics
    static void print_stats(const std::vector<KVPair>& data) {
        if (data.empty()) {
            DOCA_LOG_INFO("Data is empty");
            return;
        }
        
        DOCA_LOG_INFO("Data Statistics:");
        DOCA_LOG_INFO("  Count: %zu", data.size());
        DOCA_LOG_INFO("  Min key: %lu", data.front().key);
        DOCA_LOG_INFO("  Max key: %lu", data.back().key);
        
        // Build first 5 keys string
        std::string first5 = "  First 5 keys: ";
        for (size_t i = 0; i < std::min(size_t(5), data.size()); ++i) {
            first5 += std::to_string(data[i].key) + " ";
        }
        DOCA_LOG_INFO("%s", first5.c_str());
        
        // Build last 5 keys string
        std::string last5 = "  Last 5 keys: ";
        size_t start = (data.size() >= 5) ? data.size() - 5 : 0;
        for (size_t i = start; i < data.size(); ++i) {
            last5 += std::to_string(data[i].key) + " ";
        }
        DOCA_LOG_INFO("%s", last5.c_str());
    }
};

#endif // TEST_DATA_GENERATOR_HPP