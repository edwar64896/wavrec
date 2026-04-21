#pragma once
/*
 * Lock-free single-producer / single-consumer ring buffer.
 *
 * Usage — declare a typed ring buffer in a header:
 *
 *   RINGBUF_DECL(FloatRing, float, 4096);
 *
 * This emits a struct and four inline functions:
 *   FloatRing_init  (&rb)
 *   FloatRing_write (&rb, val)   → bool (false = full, drop)
 *   FloatRing_read  (&rb, &out)  → bool (false = empty)
 *   FloatRing_count (&rb)        → uint32_t (approximate)
 *
 * Rules:
 *   - Exactly ONE thread calls _write, exactly ONE calls _read.
 *   - Capacity MUST be a compile-time power of two.
 *   - wp and rp occupy separate cache lines to prevent false sharing.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>

#define RINGBUF_CACHE_LINE 64

#define RINGBUF_DECL(Name, T, Cap)                                              \
    _Static_assert((Cap) > 0 && ((Cap) & ((Cap) - 1)) == 0,                    \
                   #Name ": capacity must be a power of two");                  \
    typedef struct {                                                             \
        _Atomic uint32_t wp;                                                    \
        char _pad0[RINGBUF_CACHE_LINE - sizeof(_Atomic uint32_t)];              \
        _Atomic uint32_t rp;                                                    \
        char _pad1[RINGBUF_CACHE_LINE - sizeof(_Atomic uint32_t)];              \
        T items[Cap];                                                            \
    } Name;                                                                     \
                                                                                \
    static inline void Name##_init(Name *rb) {                                  \
        atomic_init(&rb->wp, 0u);                                               \
        atomic_init(&rb->rp, 0u);                                               \
    }                                                                            \
                                                                                \
    static inline bool Name##_write(Name *rb, T val) {                         \
        uint32_t w = atomic_load_explicit(&rb->wp, memory_order_relaxed);       \
        uint32_t r = atomic_load_explicit(&rb->rp, memory_order_acquire);       \
        if ((w - r) == (uint32_t)(Cap))                                         \
            return false; /* full */                                             \
        rb->items[w & ((Cap) - 1)] = val;                                       \
        atomic_store_explicit(&rb->wp, w + 1, memory_order_release);            \
        return true;                                                             \
    }                                                                            \
                                                                                \
    static inline bool Name##_read(Name *rb, T *out) {                         \
        uint32_t r = atomic_load_explicit(&rb->rp, memory_order_relaxed);       \
        uint32_t w = atomic_load_explicit(&rb->wp, memory_order_acquire);       \
        if (r == w)                                                              \
            return false; /* empty */                                            \
        *out = rb->items[r & ((Cap) - 1)];                                      \
        atomic_store_explicit(&rb->rp, r + 1, memory_order_release);            \
        return true;                                                             \
    }                                                                            \
                                                                                \
    /* Approximate — both positions loaded independently. */                    \
    static inline uint32_t Name##_count(Name *rb) {                            \
        uint32_t w = atomic_load_explicit(&rb->wp, memory_order_acquire);       \
        uint32_t r = atomic_load_explicit(&rb->rp, memory_order_acquire);       \
        return w - r; /* natural uint32 wrap handles rollover */                \
    }
