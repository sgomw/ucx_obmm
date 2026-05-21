/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_ATOMIC_H_
#define UCT_OBMM_ATOMIC_H_

#include <ucs/arch/atomic.h>
#include <ucs/sys/compiler_def.h>

#include <stdint.h>


/* Target NC mappings support cross-node atomic RMW only through explicit arm64
 * LSE instructions. Generic ucs_atomic/__sync atomics may be lowered to LL/SC
 * on aarch64, which is not usable on this hardware. Keep every obmm shared
 * control-word RMW on the explicit LSE path. */
#if defined(__aarch64__)
static UCS_F_ALWAYS_INLINE uint32_t
uct_obmm_atomic_cswap32(volatile uint32_t *ptr, uint32_t oldval,
                        uint32_t newval)
{
    __asm__ __volatile__(
            ".arch_extension lse\n\t"
            "cas %w[old], %w[new], [%[ptr]]"
            : [old] "+r" (oldval)
            : [new] "r" (newval), [ptr] "r" (ptr)
            : "memory");
    return oldval;
}


static UCS_F_ALWAYS_INLINE uint64_t
uct_obmm_atomic_cswap64(volatile uint64_t *ptr, uint64_t oldval,
                        uint64_t newval)
{
    __asm__ __volatile__(
            ".arch_extension lse\n\t"
            "cas %x[old], %x[new], [%[ptr]]"
            : [old] "+r" (oldval)
            : [new] "r" (newval), [ptr] "r" (ptr)
            : "memory");
    return oldval;
}


static UCS_F_ALWAYS_INLINE int
uct_obmm_atomic_bool_cswap64(volatile uint64_t *ptr, uint64_t oldval,
                             uint64_t newval)
{
    return uct_obmm_atomic_cswap64(ptr, oldval, newval) == oldval;
}
#else
static UCS_F_ALWAYS_INLINE uint32_t
uct_obmm_atomic_cswap32(volatile uint32_t *ptr, uint32_t oldval,
                        uint32_t newval)
{
    return ucs_atomic_cswap32(ptr, oldval, newval);
}


static UCS_F_ALWAYS_INLINE uint64_t
uct_obmm_atomic_cswap64(volatile uint64_t *ptr, uint64_t oldval,
                        uint64_t newval)
{
    return ucs_atomic_cswap64(ptr, oldval, newval);
}


static UCS_F_ALWAYS_INLINE int
uct_obmm_atomic_bool_cswap64(volatile uint64_t *ptr, uint64_t oldval,
                             uint64_t newval)
{
    return ucs_atomic_bool_cswap64(ptr, oldval, newval);
}
#endif

#endif
