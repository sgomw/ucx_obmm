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

    /* Shared FIFO elements carry bcopy metadata only. Tiny am_short uses the
     * dedicated SPSC short-lane area instead of the legacy FIFO. */
    UCT_OBMM_FIFO_ELEM_FLAG_BCOPY = UCS_BIT(1)
};

enum {
    /* Deterministic small-message SPSC lanes: one half is reserved for senders
     * from the local export region, the other half for import-side senders
     * from the peer node. This covers the current 2-node topology without
     * per-message CAS on the tiny-message fast path. */
    UCT_OBMM_SHORT_LANE_COUNT     = 64u,
    UCT_OBMM_SHORT_LANE_FIFO_SIZE = 8u,
    UCT_OBMM_SHORT_LANE_ELEM_SIZE = 64u,
    UCT_OBMM_SHORT_LANE_TAIL_BATCH = UCT_OBMM_SHORT_LANE_FIFO_SIZE / 2u
};


/* Per-slot FIFO control header. Lives at offset 0 of every allocated slot in
 * the obmm pool. Producers reserve `head` with CAS loops; on target aarch64 NC
 * mappings those CAS operations must use explicit LSE instructions, not
 * compiler-default LL/SC atomics. Consumers read/write `tail` to release
 * space. Control fields are accessed via non-cacheable mappings, therefore all
 * updates must be paired with bus fences (ucs_memory_bus_*_fence), not CPU
 * fences. */
typedef struct uct_obmm_fifo_ctl {
    /* 1st cacheline: producer-touched */
    volatile uint64_t head;
    UCS_CACHELINE_PADDING(uint64_t);

    /* 2nd cacheline: consumer-touched */
    volatile uint64_t tail;
    UCS_CACHELINE_PADDING(uint64_t);
} UCS_V_ALIGNED(UCS_SYS_CACHE_LINE_SIZE) uct_obmm_fifo_ctl_t;

typedef struct uct_obmm_short_lane_state {
    uint32_t sender_slot_index;
    uint32_t sender_generation;
    uint32_t sender_pid;
    uint32_t receiver_generation;
    uint32_t reset_generation;
    uint32_t reserved;
    volatile uint64_t tail;
    uint8_t  pad[UCS_SYS_CACHE_LINE_SIZE - 32];
} UCS_V_ALIGNED(UCS_SYS_CACHE_LINE_SIZE) uct_obmm_short_lane_state_t;


typedef struct uct_obmm_short_lane {
    uct_obmm_short_lane_state_t state;
    uint8_t elems[UCT_OBMM_SHORT_LANE_FIFO_SIZE][UCT_OBMM_SHORT_LANE_ELEM_SIZE];
} UCS_V_ALIGNED(UCS_SYS_CACHE_LINE_SIZE) uct_obmm_short_lane_t;


typedef struct uct_obmm_short_lane_table_hdr {
    volatile uint64_t active_mask;
    UCS_CACHELINE_PADDING(uint64_t);
} UCS_V_ALIGNED(UCS_SYS_CACHE_LINE_SIZE) uct_obmm_short_lane_table_hdr_t;


/* FIFO element header. In the current design the shared FIFO carries bcopy
 * metadata, while the dedicated short-lane storage reuses the same header
 * format for tiny am_short payloads. */
typedef struct uct_obmm_fifo_element {
    uint8_t  flags;       /* UCT_OBMM_FIFO_ELEM_FLAG_xx */
    uint8_t  am_id;       /* active message id */
    uint16_t length;      /* bcopy payload bytes, or [hdr|payload] bytes in
                             short-lane elements */
    uint32_t generation;  /* owner-slot generation token; receiver discards
                             elements whose generation doesn't match the
                             slot's current meta.generation */
    uint64_t header;      /* short-lane am_short header; unused for bcopy */
    /* payload[length] follows here */
} UCS_S_PACKED uct_obmm_fifo_element_t;


/* Compute slot stride: control header + fifo_size * elem_size + (v2)
 * fifo_size * bcopy_seg_size, cacheline aligned so that adjacent slots
 * don't share a line. Returned as size_t; callers must validate the
 * result fits in the uint32_t pool_hdr->slot_size field before passing
 * to pool_attach. */
static UCS_F_ALWAYS_INLINE size_t
uct_obmm_slot_stride(unsigned fifo_size, unsigned fifo_elem_size,
                     unsigned bcopy_seg_size)
{
    return ucs_align_up(sizeof(uct_obmm_fifo_ctl_t) +
                        sizeof(uct_obmm_short_lane_table_hdr_t) +
                        (UCT_OBMM_SHORT_LANE_COUNT *
                         sizeof(uct_obmm_short_lane_t)) +
                        ((size_t)fifo_size * fifo_elem_size) +
                        ((size_t)fifo_size * bcopy_seg_size),
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
    return UCS_PTR_BYTE_OFFSET(slot_base, sizeof(uct_obmm_fifo_ctl_t) +
                                          sizeof(uct_obmm_short_lane_table_hdr_t) +
                                          (UCT_OBMM_SHORT_LANE_COUNT *
                                           sizeof(uct_obmm_short_lane_t)));
}


/* Get bcopy desc array pointer from a slot base pointer. The desc area
 * lives immediately after the FIFO element array. Each desc is
 * `bcopy_seg_size` bytes; index N is paired 1:1 with FIFO element N. */
static UCS_F_ALWAYS_INLINE void*
uct_obmm_slot_descs(void *slot_base, unsigned fifo_size,
                    unsigned fifo_elem_size)
{
    return UCS_PTR_BYTE_OFFSET(slot_base,
                               sizeof(uct_obmm_fifo_ctl_t) +
                               sizeof(uct_obmm_short_lane_table_hdr_t) +
                               (UCT_OBMM_SHORT_LANE_COUNT *
                                sizeof(uct_obmm_short_lane_t)) +
                               ((size_t)fifo_size * fifo_elem_size));
}


static UCS_F_ALWAYS_INLINE volatile uint64_t*
uct_obmm_slot_short_active_mask(void *slot_base)
{
    return &((uct_obmm_short_lane_table_hdr_t*)
             UCS_PTR_BYTE_OFFSET(slot_base, sizeof(uct_obmm_fifo_ctl_t)))->active_mask;
}


static UCS_F_ALWAYS_INLINE uct_obmm_short_lane_t*
uct_obmm_slot_short_lanes(void *slot_base)
{
    return (uct_obmm_short_lane_t*)
           UCS_PTR_BYTE_OFFSET(slot_base, sizeof(uct_obmm_fifo_ctl_t) +
                                          sizeof(uct_obmm_short_lane_table_hdr_t));
}


static UCS_F_ALWAYS_INLINE uct_obmm_short_lane_t*
uct_obmm_slot_short_lane(void *slot_base, unsigned lane_index)
{
    return &uct_obmm_slot_short_lanes(slot_base)[lane_index];
}


static UCS_F_ALWAYS_INLINE uct_obmm_fifo_element_t*
uct_obmm_short_lane_elem(uct_obmm_short_lane_t *lane, uint64_t index)
{
    return (uct_obmm_fifo_element_t*)
           &lane->elems[index & (UCT_OBMM_SHORT_LANE_FIFO_SIZE - 1u)][0];
}


static UCS_F_ALWAYS_INLINE uint8_t
uct_obmm_short_lane_owner_bit(uint64_t index)
{
    return (index & UCT_OBMM_SHORT_LANE_FIFO_SIZE) ? 0u :
                                                     UCT_OBMM_FIFO_ELEM_FLAG_OWNER;
}


static UCS_F_ALWAYS_INLINE unsigned
uct_obmm_short_lane_max_short(void)
{
    return UCT_OBMM_SHORT_LANE_ELEM_SIZE -
           ucs_offsetof(uct_obmm_fifo_element_t, header);
}


static UCS_F_ALWAYS_INLINE uct_obmm_fifo_element_t*
uct_obmm_slot_elem(void *elems, uint64_t index, unsigned mask,
                   unsigned elem_size)
{
    return (uct_obmm_fifo_element_t*)
           UCS_PTR_BYTE_OFFSET(elems, (size_t)(index & mask) * elem_size);
}


static UCS_F_ALWAYS_INLINE void*
uct_obmm_slot_desc(void *descs, uint64_t index, unsigned mask,
                   unsigned seg_size)
{
    return UCS_PTR_BYTE_OFFSET(descs, (size_t)(index & mask) * seg_size);
}


/* Full bus-domain fence: orders ALL prior memory accesses (loads and
 * stores) before ALL subsequent memory accesses, in the outer-shareable /
 * device domain that includes cross-host obmm peers.
 *
 * Why we need this in addition to ucs_memory_bus_{store,load}_fence:
 * the receiver must order its desc[N] LOADS (issued during the AM
 * handler) before the STORE that publishes the new tail. On ARM64,
 * ucs_memory_bus_store_fence() is `dmb oshst` (store→store only) and
 * does not order prior loads. Without this load→store barrier, a sender
 * could observe the advanced tail and overwrite desc[N] while the
 * receiver still has outstanding loads from the previous lap's payload.
 *
 * Defined locally in obmm rather than added to ucs/arch to keep the
 * change inside ucx/src/uct/obmm/ per AGENTS.md. */
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
/* Fallback: combine store + load fences. Not strictly load→store on all
 * archs but better than nothing; build will warn so the porter notices. */
#warning "obmm: no full bus fence for this arch; receiver tail release ordering may be weak"
#define uct_obmm_bus_full_fence() do { \
    ucs_memory_bus_load_fence();        \
    ucs_memory_bus_store_fence();       \
} while (0)
#endif

#endif
