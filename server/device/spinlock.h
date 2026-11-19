#ifndef DPA_SPINLOCK_H
#define DPA_SPINLOCK_H

#include <stdbool.h>
#include <libflexio-dev/flexio_dev.h>

/* Wrapper mapped to Flex IO SDK dev spinlock helpers. */

typedef struct spinlock_s dpa_spinlock_t;

/* Initialize spinlock using Flex IO SDK primitive */
static inline void dpa_spinlock_init(dpa_spinlock_t* lock) {
    spin_init(lock);
}

/* Acquire spinlock (blocking) */
static inline void dpa_spinlock_acquire(dpa_spinlock_t* lock) {
    spin_lock(lock);
}

/* Release spinlock */
static inline void dpa_spinlock_release(dpa_spinlock_t* lock) {
    spin_unlock(lock);
}

/* Try to acquire spinlock (non-blocking) */
static inline bool dpa_spinlock_try_acquire(dpa_spinlock_t* lock) {
    return spin_trylock(lock) == 0;
}

#endif /* DPA_SPINLOCK_H */
