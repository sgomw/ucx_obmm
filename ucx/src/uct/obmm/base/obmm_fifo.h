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


/* Element flags carried in the shared FIFO element header. */
enum {
    /* Toggled every FIFO wraparound; receiver uses this to detect a freshly
     * written element without taking a tail/head delta lock. */
    UCT_OBMM_FIFO_ELEM_FLAG_OWNER = UCS_BIT(0),

    /* Set by senders that wrote via am_bcopy (pack_cb output stored in the
     * payload area starting at elem+1, with NO 8-byte am_short header
     * prefix). When clear, the element was written via am_short and
     * &elem->header + length covers the [hdr][payload] buffer. */
    UCT_OBMM_FIFO_ELEM_FLAG_BCOPY = UCS_BIT(1)
};


/* Per-slot FIFO control header. Lives at offset 0 of every allocated slot in
 * the obmm pool. The producer atomically FAA-s `head` to claim an element;
 * the consumer reads `tail` to release space. Both fields are accessed via
 * non-cacheable mappings, therefore all updates must be paired with bus
 * fences (ucs_memory_bus_*_fence), not CPU fences. */
typedef struct uct_obmm_fifo_ctl {
    /* 1st cacheline: producer-touched */
    volatile uint64_t head;
    UCS_CACHELINE_PADDING(uint64_t);

    /* 2nd cacheline: consumer-touched */
    volatile uint64_t tail;
    UCS_CACHELINE_PADDING(uint64_t);
} UCS_V_ALIGNED(UCS_SYS_CACHE_LINE_SIZE) uct_obmm_fifo_ctl_t;


/* FIFO element header. The element body (am short header + payload) follows
 * immediately. Total element stride is iface->config.fifo_elem_size, which
 * the iface chooses so that header+payload <= elem_size. */
typedef struct uct_obmm_fifo_element {
    uint8_t  flags;       /* UCT_OBMM_FIFO_ELEM_FLAG_xx */
    uint8_t  am_id;       /* active message id */
    uint16_t length;      /* payload length (excluding am_short header) */
    uint32_t generation;  /* owner-slot generation token; receiver discards
                             elements whose generation doesn't match the
                             slot's current meta.generation */
    uint64_t header;      /* am_short 64-bit header */
    /* payload[length] follows here */
} UCS_S_PACKED uct_obmm_fifo_element_t;


/* Compute slot stride: control header + fifo_size * elem_size, cacheline
 * aligned so that adjacent slots don't share a line. */
static UCS_F_ALWAYS_INLINE size_t
uct_obmm_slot_stride(unsigned fifo_size, unsigned fifo_elem_size)
{
    return ucs_align_up(sizeof(uct_obmm_fifo_ctl_t) +
                        ((size_t)fifo_size * fifo_elem_size),
                        UCS_SYS_CACHE_LINE_SIZE);
}


/* Get FIFO control header pointer from a slot base pointer. */
static UCS_F_ALWAYS_INLINE uct_obmm_fifo_ctl_t*
uct_obmm_slot_ctl(void *slot_base)
{
    return (uct_obmm_fifo_ctl_t*)slot_base;
}


/* Get FIFO elements array pointer from a slot base pointer. */
static UCS_F_ALWAYS_INLINE void*
uct_obmm_slot_elems(void *slot_base)
{
    return UCS_PTR_BYTE_OFFSET(slot_base, sizeof(uct_obmm_fifo_ctl_t));
}


static UCS_F_ALWAYS_INLINE uct_obmm_fifo_element_t*
uct_obmm_slot_elem(void *elems, uint64_t index, unsigned mask,
                   unsigned elem_size)
{
    return (uct_obmm_fifo_element_t*)
           UCS_PTR_BYTE_OFFSET(elems, (size_t)(index & mask) * elem_size);
}

#endif
