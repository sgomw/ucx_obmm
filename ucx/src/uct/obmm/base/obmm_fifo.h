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

/* Every sender-owned slot contains two mailbox banks so destination slot
 * indexes from the local host and the remote host cannot collide. */
enum {
    UCT_OBMM_MAILBOX_BANK_LOCAL  = 0u,
    UCT_OBMM_MAILBOX_BANK_REMOTE = 1u,
    UCT_OBMM_MAILBOX_BANK_COUNT  = 2u
};


/* Element flags carried in the shared mailbox element header. */
enum {
    UCT_OBMM_MAILBOX_ELEM_FLAG_BCOPY = UCS_BIT(0)
};


/* Per-lane mailbox control. One sender owns head/sender_generation; one
 * receiver owns tail/tail_generation. The slot owner initializes all lanes'
 * sender_generation to its slot generation when the slot is allocated. */
typedef struct uct_obmm_mailbox_ctl {
    /* 1st cacheline: sender-touched */
    volatile uint32_t head;
    volatile uint32_t sender_generation;
    UCS_CACHELINE_PADDING(uint32_t, uint32_t);

    /* 2nd cacheline: receiver-touched */
    volatile uint32_t tail;
    volatile uint32_t tail_generation;
    UCS_CACHELINE_PADDING(uint32_t, uint32_t);
} UCS_V_ALIGNED(UCS_SYS_CACHE_LINE_SIZE) uct_obmm_mailbox_ctl_t;


/* Mailbox element header. The element body follows immediately. For am_short,
 * the callback data is [header|payload] starting at &elem->header. For
 * am_bcopy, the callback data lives in the paired desc entry. */
typedef struct uct_obmm_fifo_element {
    uint8_t  flags;       /* UCT_OBMM_MAILBOX_ELEM_FLAG_xx */
    uint8_t  am_id;       /* active message id */
    uint16_t length;      /* bytes passed to the AM callback */
    uint32_t generation;  /* receiver-slot generation token; receiver discards
                             elements whose generation doesn't match its local
                             iface generation */
    uint64_t header;      /* am_short 64-bit header */
    /* payload[length] follows here */
} UCS_S_PACKED uct_obmm_fifo_element_t;


static UCS_F_ALWAYS_INLINE size_t
uct_obmm_mailbox_lane_stride(unsigned fifo_size, unsigned fifo_elem_size,
                             unsigned bcopy_seg_size)
{
    return ucs_align_up(sizeof(uct_obmm_mailbox_ctl_t) +
                        ((size_t)fifo_size * fifo_elem_size) +
                        ((size_t)fifo_size * bcopy_seg_size),
                        UCS_SYS_CACHE_LINE_SIZE);
}


/* Compute slot stride: every slot contains one mailbox lane for every
 * (bank, destination-slot-index) pair. Returned as size_t; callers must still
 * validate it fits in the uint32_t pool header field. */
static UCS_F_ALWAYS_INLINE size_t
uct_obmm_slot_stride(unsigned fifo_size, unsigned fifo_elem_size,
                     unsigned bcopy_seg_size)
{
    return ucs_align_up((size_t)UCT_OBMM_MAILBOX_BANK_COUNT *
                        UCT_OBMM_POOL_SLOT_COUNT *
                        uct_obmm_mailbox_lane_stride(fifo_size, fifo_elem_size,
                                                     bcopy_seg_size),
                        UCS_SYS_CACHE_LINE_SIZE);
}


static UCS_F_ALWAYS_INLINE void*
uct_obmm_slot_lane(void *slot_base, unsigned bank, unsigned dst_slot_index,
                   unsigned fifo_size, unsigned fifo_elem_size,
                   unsigned bcopy_seg_size)
{
    size_t lane_index = ((size_t)bank * UCT_OBMM_POOL_SLOT_COUNT) +
                        dst_slot_index;
    size_t lane_stride = uct_obmm_mailbox_lane_stride(fifo_size, fifo_elem_size,
                                                      bcopy_seg_size);

    return UCS_PTR_BYTE_OFFSET(slot_base, lane_index * lane_stride);
}


static UCS_F_ALWAYS_INLINE uct_obmm_mailbox_ctl_t*
uct_obmm_lane_ctl(void *lane_base)
{
    return (uct_obmm_mailbox_ctl_t*)lane_base;
}


static UCS_F_ALWAYS_INLINE void*
uct_obmm_lane_elems(void *lane_base)
{
    return UCS_PTR_BYTE_OFFSET(lane_base, sizeof(uct_obmm_mailbox_ctl_t));
}


static UCS_F_ALWAYS_INLINE void*
uct_obmm_lane_descs(void *lane_base, unsigned fifo_size,
                    unsigned fifo_elem_size)
{
    return UCS_PTR_BYTE_OFFSET(lane_base,
                               sizeof(uct_obmm_mailbox_ctl_t) +
                               ((size_t)fifo_size * fifo_elem_size));
}


static UCS_F_ALWAYS_INLINE uct_obmm_fifo_element_t*
uct_obmm_lane_elem(void *elems, uint32_t index, unsigned mask,
                   unsigned elem_size)
{
    return (uct_obmm_fifo_element_t*)
           UCS_PTR_BYTE_OFFSET(elems, (size_t)(index & mask) * elem_size);
}


static UCS_F_ALWAYS_INLINE void*
uct_obmm_lane_desc(void *descs, uint32_t index, unsigned mask,
                   unsigned seg_size)
{
    return UCS_PTR_BYTE_OFFSET(descs, (size_t)(index & mask) * seg_size);
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
#warning "obmm: no full bus fence for this arch; mailbox ack ordering may be weak"
#define uct_obmm_bus_full_fence() do { \
    ucs_memory_bus_load_fence();        \
    ucs_memory_bus_store_fence();       \
} while (0)
#endif

#endif
