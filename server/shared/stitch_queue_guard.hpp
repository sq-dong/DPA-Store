#pragma once

#include <cstdint>

#include "host_global_area.h"

/* Thread-safe wrapper for publishing stitch commands from multiple patchers. */
int host_send_stitch_thread_safe(host_global_area_t *hga, uint32_t queue_id, const void *cmd, uint32_t size, bool print_debug = false);
