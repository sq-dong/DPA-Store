#include "target_interface.hpp"
#include "config.hpp"

#include <chrono>
#include <iostream>
#include <thread>

void target_on_benchmark_ready(int argc, char **argv) {

}

void target_thread_init(std::size_t thread_id) {
    std::cout << "[test-target] thread " << thread_id << " init (NUMBER_THREADS="
              << NUMBER_THREADS << ")" << '\n';
}

OperationResult target_do_operation(std::size_t thread_id,
                                    std::size_t bucket_id,
                                    const Operation& operation) {
    auto start = std::chrono::steady_clock::now();

    // Simulate a small amount of work for the test target.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    const auto latency_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             start)
            .count();

    std::cout << "[test-target] thread " << thread_id << " bucket " << bucket_id
              << " completed operation " << operation.id << '\n';

    return OperationResult{OperationStatus::Finished, static_cast<std::uint64_t>(latency_ns)};
}

std::vector<std::size_t> target_check_for_replies(std::size_t) {
    return {};
}

void target_thread_exit(std::size_t thread_id) {
    std::cout << "[test-target] thread " << thread_id << " exit" << '\n';
}
