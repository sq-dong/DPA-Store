#include <doca_log.h>
DOCA_LOG_REGISTER(STITCH_QUEUE)

#include "stitch_queue_guard.hpp"

#include "spin_lock.hpp"

#include <array>

namespace {
std::array<SpinLock, NUM_STITCH_QUEUES> g_stitch_locks;
} // namespace

int host_send_stitch_thread_safe(host_global_area_t *hga, uint32_t queue_id, const void *cmd, uint32_t size, bool print_debug) {
    uint32_t queue = NUM_STITCH_QUEUES ? (queue_id % NUM_STITCH_QUEUES) : 0;
    SpinLockGuard guard(g_stitch_locks[queue]);
#if DEBUG_INSERTS_MULTI_PATCHER == 1
    if (print_debug) { DOCA_LOG_INFO("[stitch] host_send_stitch_thread_safe after lock)"); }
#endif // DEBUG_INSERTS_MULTI_PATCHER == 1
    return host_send_stitch(hga, queue, cmd, size);
}
