#pragma once

// Global defaults for benchmark configuration, targets can override via target_config.hpp.

#define NUMBER_THREADS 4
#define TARGET_NAME "placeholder"
#define OUTSTANDING_OPS_PER_THREAD 4
#define OPERATIONS_PER_THREAD 16


#define DEFAULT_THREADED 0
#define CUSTOM_THREADED 1
#define FULL_CUSTOM 2
#define CONCURRENCY_MODE DEFAULT_THREADED
