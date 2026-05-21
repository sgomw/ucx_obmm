/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_FIFO_H_
#define UCT_OBMM_FIFO_H_

#include <ucs/arch/cpu.h>
#include <ucs/sys/compiler.h>
#include <ucs/sys/compiler_def.h>
#include <ucs/sys/ptr_arith.h>

#include <stdint.h>


/* Number of iface-owned slots in the per-region pool. Caps how many ifaces can
 * attach to a single obmm region from one host. */
#define UCT_OBMM_POOL_SLOT_COUNT 32u

/* Receiver-local FIFO layout identifier carried in iface addresses so peers do
 * not confuse this slot layout with the older sender-owned mailbox design. */
#define UCT_OBMM_FIFO_LAYOUT_ATOMIC_SHARDED 2u

/* Every receiver-owned slot contains two banks so sender slot indexes from the
 * local host and the remote host cannot collide. */
enum {
    UCT_OBMM_FIFO_BANK_LOCAL  = 0u,
    UCT_OBMM_FIFO_BANK_REMOTE = 1u,
    UCT_OBMM_FIFO_BANK_COUNT  = 2u,
    UCT_OBMM_MAX_FIFO_SHARDS  = UCT_OBMM_POOL_SLOT_COUNT
};


/* Element flags carried in the shared FIFO element header. */
enum {
    UCT_OBMM_FIFO_ELEM_FLAG_OWNER = UCS_BIT(0),
    UCT_OBMM_FIFO_ELEM_FLAG_BCOPY = UCS_BIT(1)
};


/* Per-shard FIFO control. Multiple senders CAS-reserve head; the local
 * receiver alone advances tail. receiver_generation records which slot
 * generation initialized this shard layout. */
typedef struct uct_obmm_fifo_ctl {
    /* 1st cacheline: sender-touched */
    volatile uint64_t head;
    UCS_CACHELINE_PADDING(uint64_t);

    /* 2nd cacheline: receiver-touched */
    volatile uint64_t tail;
    volatile uint32_t receiver_generation;
    uint32_t          reserved;
    UCS_CACHELINE_PADDING(uint64_t, uint32_t, uint32_t);
} UCS_V_ALIGNED(UCS_SYS_CACHE_LINE_SIZE) uct_obmm_fifo_ctl_t;


/* Shared FIFO element header. For am_short the callback data is
 * [header|payload] starting at &elem->header. For am_bcopy the callback data
 * lives in the paired desc entry. The owner bit is the mm-style publish bit
 * that flips every ring wrap. */
typedef struct uct_obmm_fifo_element {
    uint8_t  flags;       /* UCT_OBMM_FIFO_ELEM_FLAG_xx */
    uint8_t  am_id;       /* active message id */
    uint16_t length;      /* bytes passed to the AM callback */
    uint32_t generation;  /* receiver-slot generation token; receiver discards
                             elements whose generation doesn't match its local
                             iface generation */
    uint64_t header;      /* am_short 64-bit header */
    /* payload[length] follows here */
} UCS_S_PACKED uct_obmm_fifo_element_t;


static UCS_F_ALWAYS_INLINE uint64_t
uct_obmm_atomic_cswap64(volatile uint64_t *ptr, uint64_t oldval,
                        uint64_t newval)
{
#if defined(__aarch64__)
    uint64_t observed = oldval;

    __asm__ __volatile__(
            ".arch_extension lse\n\t"
            "casal %x[observed], %x[newval], [%[ptr]]"
            : [observed] "+&r"(observed)
            : [newval] "r"(newval), [ptr] "r"(ptr)
            : "memory");
    return observed;
#else
    return __sync_val_compare_and_swap(ptr, oldval, newval);
#endif
}


static UCS_F_ALWAYS_INLINE size_t
uct_obmm_fifo_shard_stride(unsigned fifo_size, unsigned fifo_elem_size,
                           unsigned bcopy_seg_size)
{
    return ucs_align_up(sizeof(uct_obmm_fifo_ctl_t) +
                        ((size_t)fifo_size * fifo_elem_size) +
                        ((size_t)fifo_size * bcopy_seg_size),
                        UCS_SYS_CACHE_LINE_SIZE);
}


/* Compute slot stride: every slot contains one FIFO shard for every
 * (bank, shard-index) pair. Returned as size_t; callers must still validate it
 * fits in the uint32_t pool header field. */
static UCS_F_ALWAYS_INLINE size_t
uct_obmm_slot_stride(unsigned shard_count, unsigned fifo_size,
                     unsigned fifo_elem_size, unsigned bcopy_seg_size)
{
    return ucs_align_up((size_t)UCT_OBMM_FIFO_BANK_COUNT * shard_count *
                        uct_obmm_fifo_shard_stride(fifo_size, fifo_elem_size,
                                                   bcopy_seg_size),
                        UCS_SYS_CACHE_LINE_SIZE);
}


static UCS_F_ALWAYS_INLINE void*
uct_obmm_slot_shard(void *slot_base, unsigned bank, unsigned shard_index,
                    unsigned shard_count, unsigned fifo_size,
                    unsigned fifo_elem_size, unsigned bcopy_seg_size)
{
    size_t index  = ((size_t)bank * shard_count) + shard_index;
    size_t stride = uct_obmm_fifo_shard_stride(fifo_size, fifo_elem_size,
                                               bcopy_seg_size);

    return UCS_PTR_BYTE_OFFSET(slot_base, index * stride);
}


static UCS_F_ALWAYS_INLINE uct_obmm_fifo_ctl_t*
uct_obmm_shard_ctl(void *shard_base)
{
    return (uct_obmm_fifo_ctl_t*)shard_base;
}


static UCS_F_ALWAYS_INLINE void*
uct_obmm_shard_elems(void *shard_base)
{
    return UCS_PTR_BYTE_OFFSET(shard_base, sizeof(uct_obmm_fifo_ctl_t));
}


static UCS_F_ALWAYS_INLINE void*
uct_obmm_shard_descs(void *shard_base, unsigned fifo_size,
                     unsigned fifo_elem_size)
{
    return UCS_PTR_BYTE_OFFSET(shard_base,
                               sizeof(uct_obmm_fifo_ctl_t) +
                               ((size_t)fifo_size * fifo_elem_size));
}


static UCS_F_ALWAYS_INLINE uct_obmm_fifo_element_t*
uct_obmm_shard_elem(void *elems, uint64_t index, unsigned mask,
                    unsigned elem_size)
{
    return (uct_obmm_fifo_element_t*)
           UCS_PTR_BYTE_OFFSET(elems, (size_t)(index & mask) * elem_size);
}


static UCS_F_ALWAYS_INLINE void*
uct_obmm_shard_desc(void *descs, uint64_t index, unsigned mask,
                    unsigned seg_size)
{
    return UCS_PTR_BYTE_OFFSET(descs, (size_t)(index & mask) * seg_size);
}


static UCS_F_ALWAYS_INLINE uint8_t
uct_obmm_fifo_owner_bit(uint64_t index, unsigned fifo_size)
{
    return (index & fifo_size) ? UCT_OBMM_FIFO_ELEM_FLAG_OWNER : 0;
}


static UCS_F_ALWAYS_INLINE int
uct_obmm_fifo_elem_is_ready(const uct_obmm_fifo_element_t *elem, uint64_t index,
                            unsigned fifo_size)
{
    return ((elem->flags & UCT_OBMM_FIFO_ELEM_FLAG_OWNER) ==
            uct_obmm_fifo_owner_bit(index, fifo_size));
}


static UCS_F_ALWAYS_INLINE int
uct_obmm_fifo_has_space(uint64_t head, uint64_t tail, unsigned fifo_size)
{
    return (head - tail) < fifo_size;
}


/* Full bus-domain fence: orders ALL prior memory accesses (loads and stores)
 * before ALL subsequent memory accesses, in the outer-shareable / device
 * domain that includes cross-host obmm peers. */
#if defined(__aarch64__)
#define uct_obmm_bus_full_fence() __asm__ __volatile__("dmb osh" ::: "memory")
#elif defined(__x86_64__) || defined(__i386__)
#define uct_obmm_bus_full_fence() __asm__ __volatile__("mfence" ::: "memory")
#elif defined(__powerpc64__)
#define uct_obmm_bus_full_fence() __asm__ __volatile__("sync" ::: "memory")
#elif defined(__riscv) && (__riscv_xlen == 64)
#define uct_obmm_bus_full_fence() \
    __asm__ __volatile__("fence iorw, iorw" ::: "memory")
#else
#warning "obmm: no full bus fence for this arch; FIFO tail ordering may be weak"
#define uct_obmm_bus_full_fence() do { \
    ucs_memory_bus_load_fence();        \
    ucs_memory_bus_store_fence();       \
} while (0)
#endif

#endif
