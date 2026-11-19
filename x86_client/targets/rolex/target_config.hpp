#pragma once

// ----------------------------------------------------------------------------
// Multiclient overrides for the ROLEX target.
// All configuration that would normally come from command-line flags in the
// standalone ROLEX client must be provided here via macros.
// ----------------------------------------------------------------------------

#undef TARGET_NAME
#define TARGET_NAME "ROLEX"

#undef CONCURRENCY_MODE
#define CONCURRENCY_MODE FULL_CUSTOM

// Benchmark harness configuration.
#undef NUMBER_THREADS
#define NUMBER_THREADS 4

#undef OPERATIONS_PER_THREAD
#define OPERATIONS_PER_THREAD 12

// ROLEX runtime configuration (mirrors the gflags expected by Rolex).
// CONFIGURE FOR YOUR SITE: lab default is oscar (137.226.133.237).  Override
// at runtime with -dst_ip=<server>.
#define ROLEX_SERVER_HOSTNAME "137.226.133.237"
#define ROLEX_SERVER_PORT 8888
#define ROLEX_REGISTERED_LEAF_COUNT 10000000ULL
#define ROLEX_NIC_INDEX 0
#define ROLEX_THREAD_COUNT NUMBER_THREADS
#define ROLEX_COROUTINE_COUNT 12

// Benchmark dataset / workload configuration.
#define ROLEX_BENCHMARK_NUM_KEYS 100000ULL
#define ROLEX_BENCHMARK_NUM_NON_KEYS 100000ULL
#define ROLEX_BENCHMARK_WORKLOAD "normal"
#define ROLEX_BENCHMARK_MEM_THREADS 3ULL
#define ROLEX_BENCHMARK_SERVER_THREADS 24ULL
#define ROLEX_BENCHMARK_READ_RATIO 1.0
#define ROLEX_BENCHMARK_INSERT_RATIO 0.0
#define ROLEX_BENCHMARK_UPDATE_RATIO 0.0

// Connection / coroutine behaviour.
#define ROLEX_CONNECTION_RETRY_SECONDS 10
#define ROLEX_THREAD_MEMORY_BYTES (16 * 1024 * 1024ULL)
#define ROLEX_TRANSPORT_BUFFER_BYTES 4096
#define ROLEX_TRANSPORT_INITIAL_RECVS 2048
#define ROLEX_TRANSPORT_REPLY_BYTES 2048
