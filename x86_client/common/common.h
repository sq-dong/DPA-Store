#pragma once

#include <chrono>

inline uint64_t get_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

inline uint64_t get_delta_ns(const uint64_t before) {
    return get_time_ns() - before;
}
