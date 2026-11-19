#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <stdint.h>
#include "server_config.h"

/* Only include string.h for memcpy when not on DPA device */
#ifndef __DEVICE__
#include <string.h>
#include <stdio.h>
#endif

/* Ring buffer metadata structure
 * head: Producer writes here, advances after write
 * tail: Consumer reads here, advances after read
 * Empty when head == tail
 * Full when next write would make head == tail */
typedef struct {
    volatile uint64_t head;    /* Producer position */
    volatile uint64_t tail;    /* Consumer position */
} ringbuffer_metadata_t;

/* Initialize ringbuffer metadata
 * Sets head and tail to 0 (empty state)
 * Returns 1 on success (always succeeds) */
static inline int ringbuffer_init(ringbuffer_metadata_t *metadata, uint64_t buffer_size) {
    (void)buffer_size;  /* Unused parameter, kept for API consistency */
    metadata->head = 0;
    metadata->tail = 0;
    return 1;
}

/* Align to cache line (64 bytes) */
static inline uint64_t align_to_cache_line(uint64_t size) {
    return (size + 63) & ~63ULL;
}

/* Memory copy wrapper for DPA compatibility */
static inline void ringbuffer_memcpy(void *dest, const void *src, uint32_t size) {
#ifdef __DEVICE__
    /* DPA device doesn't have memcpy, do byte-by-byte copy */
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < size; i++) {
        d[i] = s[i];
    }
#else
    memcpy(dest, src, size);
#endif
}

/* Acquire space for producer to write directly into the ringbuffer payload.
 * Returns 1 on success, 0 if there is not enough contiguous space available.
 * On success, data_ptr points at the payload region (immediately after the
 * size header). Callers must invoke ringbuffer_produce_commit exactly once
 * after populating the payload and before attempting another acquire. */
static inline int ringbuffer_produce_acquire(ringbuffer_metadata_t *metadata,
                                             uint8_t *buffer,
                                             uint64_t buffer_size,
                                             uint32_t size,
                                             void **data_ptr) {
    uint64_t element_size = align_to_cache_line(sizeof(uint32_t) + size);

    if (element_size > buffer_size) {
        return 0;
    }

    uint64_t head = metadata->head;
    uint64_t tail = metadata->tail;

    uint64_t space_used;
    if (head >= tail) {
        space_used = head - tail;
    } else {
        space_used = buffer_size - tail + head;
    }

    uint64_t space_free = buffer_size - space_used;

    if (space_free <= element_size) {
        return 0;
    }

    uint64_t write_offset;
    if (head + element_size <= buffer_size) {
        write_offset = head;
    } else {
        if (tail <= element_size) {
            return 0;
        }

        /* Emit a wrap marker so the consumer can detect the end-of-buffer and
         * reset its tail back to zero before reading the next element. */
        *(uint32_t *)(buffer + head) = 0;
        metadata->head = 0;

        write_offset = 0;
    }

    *data_ptr = buffer + write_offset + sizeof(uint32_t);
    return 1;
}

/* Commit a previously acquired produce slot after the payload is populated.
 * data_ptr must match the pointer returned from ringbuffer_produce_acquire. */
static inline void ringbuffer_produce_commit(ringbuffer_metadata_t *metadata,
                                             uint8_t *buffer,
                                             uint64_t buffer_size,
                                             void *data_ptr,
                                             uint32_t size) {
    uint64_t element_size = align_to_cache_line(sizeof(uint32_t) + size);
    uint8_t *payload_ptr = (uint8_t *)data_ptr;
    uint8_t *entry_base = payload_ptr - sizeof(uint32_t);

    *(uint32_t *)entry_base = size;

    uint64_t entry_offset = (uint64_t)(entry_base - buffer);
    uint64_t new_head = entry_offset + element_size;

    if (new_head < buffer_size) {
        metadata->head = new_head;
    } else if (new_head == buffer_size) {
        metadata->head = 0;
    } else {
        /* Should not happen, but wrap to maintain safety */
        metadata->head = new_head - buffer_size;
    }
}

/* Producer: Add element to ring buffer
 * Returns 1 on success, 0 if full
 * Format: [4-byte size][data][padding to 64-byte alignment] */
static inline int ringbuffer_produce(ringbuffer_metadata_t *metadata, uint8_t *buffer,
                                    uint64_t buffer_size, const void *data, uint32_t size) {

    void *slot_ptr;
    if (!ringbuffer_produce_acquire(metadata, buffer, buffer_size, size, &slot_ptr)) {
        return 0;
    }

    ringbuffer_memcpy(slot_ptr, data, size);
    ringbuffer_produce_commit(metadata, buffer, buffer_size, slot_ptr, size);
    return 1;
}

/* Consumer: Read and remove element from ring buffer
 * Returns 1 on success, 0 if empty */
static inline int ringbuffer_consume(ringbuffer_metadata_t *metadata, uint8_t *buffer,
                                    uint64_t buffer_size, void *data, uint32_t max_size,
                                    uint32_t *actual_size) {

    /* Read current state */
    uint64_t head = metadata->head;
    uint64_t tail = metadata->tail;

    /* Check if empty */
    if (tail == head) {
        return 0;
    }

    /* Handle wrap case - if tail is near end without space for header */
    if (tail > buffer_size - sizeof(uint32_t)) {
        /* Must wrap to beginning */
        tail = 0;
        metadata->tail = 0;
        /* Re-check empty */
        if (tail == head) {
            return 0;
        }
    }

    /* Read size at tail */
    uint32_t size = *(uint32_t *)(buffer + tail);

    /* Validate size */
    if (size == 0 || size > max_size || size > buffer_size - sizeof(uint32_t)) {
        /* Invalid size - data corruption or wrap issue */
        /* Try wrapping to 0 if not already there */
        if (tail != 0) {
            tail = 0;
            metadata->tail = 0;
            if (tail == head) {
                return 0;
            }
            size = *(uint32_t *)(buffer);
            if (size == 0 || size > max_size || size > buffer_size - sizeof(uint32_t)) {
                return 0;  /* Still invalid */
            }
        } else {
            return 0;  /* Already at 0, can't recover */
        }
    }

    /* Copy data */
    ringbuffer_memcpy(data, buffer + tail + sizeof(uint32_t), size);
    *actual_size = size;

    /* Advance tail */
    uint64_t element_size = align_to_cache_line(sizeof(uint32_t) + size);
    uint64_t new_tail = tail + element_size;

    if (new_tail >= buffer_size) {
        metadata->tail = 0;
    } else {
        metadata->tail = new_tail;
    }

    return 1;
}

/* Peek at next element without removing it
 * Returns 1 on success, 0 if empty */
static inline int ringbuffer_peek(ringbuffer_metadata_t *metadata, uint8_t *buffer,
                                  uint64_t buffer_size, void **data_ptr, uint32_t *size) {

    /* Read current state */
    uint64_t head = metadata->head;
    uint64_t tail = metadata->tail;

    /* Check if empty */
    if (tail == head) {
        return 0;
    }

    /* Handle wrap case */
    if (tail > buffer_size - sizeof(uint32_t)) {
        tail = 0;
        metadata->tail = 0;
        if (tail == head) {
            return 0;
        }
    }

    /* Read size at tail */
    uint32_t elem_size = *(uint32_t *)(buffer + tail);

    /* Validate size */
    if (elem_size == 0 || elem_size > buffer_size - sizeof(uint32_t)) {
        /* Try wrapping */
        if (tail != 0) {
            tail = 0;
            metadata->tail = 0;
            if (tail == head) {
                return 0;
            }
            elem_size = *(uint32_t *)(buffer);
            if (elem_size == 0 || elem_size > buffer_size - sizeof(uint32_t)) {
                return 0;
            }
        } else {
            return 0;
        }
    }

    /* Return pointer to data */
    *data_ptr = buffer + tail + sizeof(uint32_t);
    *size = elem_size;

    return 1;
}

/* Commit consumption after processing peeked element */
static inline void ringbuffer_consume_commit(ringbuffer_metadata_t *metadata,
                                            uint64_t buffer_size, uint32_t size) {

    /* Current tail */
    uint64_t tail = metadata->tail;

    /* Calculate element size with alignment */
    uint64_t element_size = align_to_cache_line(sizeof(uint32_t) + size);

    /* Advance tail */
    uint64_t new_tail = tail + element_size;

    if (new_tail >= buffer_size) {
        metadata->tail = 0;
    } else {
        metadata->tail = new_tail;
    }
}

#endif /* RINGBUFFER_H */
