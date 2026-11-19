#pragma once

#include "config.hpp"
#include "backend.hpp"
#include "target_interface.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <thread>
#include <vector>

inline void run_client(int argc, char **argv) {
    std::cout << "[multiclient] rank " << global_rank()
              << "/" << global_world_size() << '\n';

    if (target_on_benchmark_ready(argc, argv)) {
        return;
    }

#if CONCURRENCY_MODE == FULL_CUSTOM
    target_on_benchmark_run();
#elif CONCURRENCY_MODE == DEFAULT_THREADED || CONCURRENCY_MODE == CUSTOM_THREADED
    if (load_traces("") == 0) {
        std::cout << "No files loaded." << std::endl;
        return;
    }

    std::vector<std::thread> workers;
    workers.reserve(NUMBER_THREADS);

    for (std::size_t thread_id = 0; thread_id < NUMBER_THREADS; ++thread_id) {
        workers.emplace_back([thread_id]() {
            target_thread_init(thread_id);
            try {
#if CONCURRENCY_MODE == DEFAULT_THREADED
                backend_default_thread_execute(thread_id);
#elif CONCURRENCY_MODE == CUSTOM_THREADED
                target_thread_execute(thread_id);
#endif
            } catch (const std::exception& ex) {
                std::cerr << "[client] thread " << thread_id
                          << " encountered exception: " << ex.what() << '\n';
            } catch (...) {
                std::cerr << "[client] thread " << thread_id
                          << " encountered unknown exception" << '\n';
            }
            target_thread_exit(thread_id);
        });
    }

    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

#endif // CONCURRENCY_MODE == DEFAULT_THREADED || CONCURRENCY_MODE == CUSTOM_THREADED
}
