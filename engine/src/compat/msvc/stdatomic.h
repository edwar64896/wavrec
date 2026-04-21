#pragma once
/*
 * stdatomic.h compatibility shim for MSVC (VS2019 and earlier).
 *
 * Valid for x86/x64 only: the TSO memory model means every load already has
 * acquire semantics and every store already has release semantics at the CPU
 * level.  Compiler barriers (_ReadBarrier / _WriteBarrier) are sufficient to
 * prevent the compiler from reordering across acquire/release points.
 *
 * This shim covers exactly the operations used in this codebase.
 * It is NOT a complete C11 stdatomic implementation.
 */

#include <intrin.h>
#include <stdint.h>

#pragma intrinsic(_ReadBarrier, _WriteBarrier)
#pragma intrinsic(_InterlockedExchangeAdd)
#pragma intrinsic(_InterlockedExchange)

/* Map _Atomic T to volatile T.  Used as a type qualifier in structs:
 *   _Atomic uint32_t wp;   →  volatile uint32_t wp; */
#define _Atomic volatile

/* Memory order enum — values match the C11 standard. */
typedef enum {
    memory_order_relaxed = 0,
    memory_order_consume = 1,
    memory_order_acquire = 2,
    memory_order_release = 3,
    memory_order_acq_rel = 4,
    memory_order_seq_cst = 5,
} memory_order;

/* atomic_init — plain assignment, called before concurrent access. */
#define atomic_init(ptr, val) (*(ptr) = (val))

/* atomic_load_explicit — insert a read barrier for acquire+ orders. */
#define atomic_load_explicit(ptr, order)   \
    ( ((order) >= memory_order_acquire)    \
        ? (_ReadBarrier(), *(ptr))         \
        : *(ptr) )

/* atomic_store_explicit — insert a write barrier for release+ orders. */
#define atomic_store_explicit(ptr, val, order) \
    do {                                        \
        *(ptr) = (val);                         \
        if ((order) >= memory_order_release)    \
            _WriteBarrier();                    \
    } while (0)

/* atomic_fetch_add / atomic_fetch_add_explicit
 * Only 32-bit supported here; extend if 64-bit add is ever needed. */
#define atomic_fetch_add(ptr, val) \
    _InterlockedExchangeAdd((volatile long *)(ptr), (long)(val))

#define atomic_fetch_add_explicit(ptr, val, order) \
    atomic_fetch_add(ptr, val)

/* atomic_compare_exchange_weak_explicit
 * Returns non-zero (true) if the exchange succeeded.
 * *expected is updated to the current value on failure. */
#define atomic_compare_exchange_weak_explicit(ptr, expected, desired, succ, fail) \
    ( _InterlockedCompareExchange((volatile long *)(ptr), (long)(desired), *(long *)(expected)) \
        == *(long *)(expected) \
      ? 1 \
      : ( *(long *)(expected) = *(volatile long *)(ptr), 0 ) )
