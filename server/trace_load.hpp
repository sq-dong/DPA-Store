#ifndef TRACE_LOAD_HPP
#define TRACE_LOAD_HPP

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <doca_log.h>
#include "../common/dpastore_common.h"

#ifndef DPASTORE_BULK_LOAD_FROM_TRACE
#define DPASTORE_BULK_LOAD_FROM_TRACE 1
#endif

#ifndef DPASTORE_BULK_LOAD_TRACE_PATH
// CONFIGURE: set to the absolute path of your bulk-load trace file, or pass
// -DDPASTORE_BULK_LOAD_TRACE_PATH=... to cmake.  If the file is not found the
// server falls back to generating synthetic data automatically.
#define DPASTORE_BULK_LOAD_TRACE_PATH "/tmp/dpastore_bulk.trace"
#endif

#ifndef DPASTORE_BULK_LOAD_TRUST_UNIQUE_ENTRIES
#define DPASTORE_BULK_LOAD_TRUST_UNIQUE_ENTRIES 1
#endif

static inline bool dpastore_load_trace_for_bulk(std::vector<KVPair>& out, const char* path) {
    std::ifstream input(path);
    if (!input) {
        DOCA_LOG_ERR("Failed to open trace file %s", path);
        return false;
    }

#if !DPASTORE_BULK_LOAD_TRUST_UNIQUE_ENTRIES
    std::unordered_map<uint64_t, uint64_t> latest;
#else
    std::vector<KVPair> entries;
#endif
    std::string line;
    size_t line_no = 0;
    size_t insert_total = 0;

    while (std::getline(input, line)) {
        line_no++;
        if (line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        std::string op;
        if (!(iss >> op)) {
            continue;
        }
        if (op != "INSERT") {
            continue;
        }
        uint64_t key = 0;
        uint64_t value = 0;
        if (!(iss >> key >> value)) {
            DOCA_LOG_WARN("Malformed INSERT line %zu in %s", line_no, path);
            continue;
        }
        // if (key < DPASTORE_BULK_LOAD_MIN_KEY || key > DPASTORE_BULK_LOAD_MAX_KEY) {
        //     continue;
        // }
#if DPASTORE_BULK_LOAD_TRUST_UNIQUE_ENTRIES
        entries.push_back(KVPair{key, value});
#else
        latest[key] = value;
#endif
        insert_total++;
    }

    out.clear();
#if DPASTORE_BULK_LOAD_TRUST_UNIQUE_ENTRIES
    if (entries.empty()) {
        DOCA_LOG_WARN("Trace %s contained no usable INSERT operations", path);
        return false;
    }
    std::sort(entries.begin(), entries.end(), [](const KVPair& lhs, const KVPair& rhs) {
        return lhs.key < rhs.key;
    });
    out.swap(entries);
    const size_t kept = out.size();
#else
    if (latest.empty()) {
        DOCA_LOG_WARN("Trace %s contained no usable INSERT operations", path);
        return false;
    }
    out.reserve(latest.size());
    for (const auto& kv : latest) {
        out.push_back(KVPair{kv.first, kv.second});
    }
    std::sort(out.begin(), out.end(), [](const KVPair& lhs, const KVPair& rhs) {
        return lhs.key < rhs.key;
    });
    const size_t kept = out.size();
#endif

    DOCA_LOG_INFO("Trace bulk load: parsed %zu lines, %zu INSERT lines, kept %zu unique keys", line_no, insert_total, kept);
    return true;
}

#endif // TRACE_LOAD_HPP
