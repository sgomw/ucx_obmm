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

    /* Shared FIFO elements carry bcopy metadata when set; otherwise the same
     * element carries inline am_short [header|payload] data. */
    UCT_OBMM_FIFO_ELEM_FLAG_BCOPY = UCS_BIT(1),
};

enum {
    /* Number of slots in the per-region pool. Caps how many ifaces can
     * attach to a single OBMM plane export region from this host. */
    UCT_OBMM_POOL_SLOT_COUNT = 96u,

    /* Pool header has no magic word. FIFO elements carry short data at byte
     * 16, with an isolated metadata prefix. Slot reuse relies on zeroing slot
     * bytes; no per-slot generation token is carried. */
    UCT_OBMM_WIRE_FORMAT_SINGLE_TLS = 12u,
    UCT_OBMM_WIRE_FORMAT_CURRENT = UCT_OBMM_WIRE_FORMAT_SINGLE_TLS,

    UCT_OBMM_FIFO_SHORT_DATA_OFFSET = 16u,
    UCT_OBMM_FIFO_BCOPY_DATA_OFFSET = 64u,

    /* Keep the advertised short capability at the former boundary while
     * preserving the byte-16 physical layout. With the default geometry, this
     * matches the physical FIFO space (FIFO_ELEM_SIZE - 16). */
    UCT_OBMM_FIFO_MAX_SHORT = 131184u,
};


/* Per-slot FIFO control header. Lives at offset 0 of every allocated slot in
 * the obmm pool. Producers reserve `head` with CAS loops; on target aarch64 NC
 * mappings those CAS operations must use explicit LSE instructions, not
 * compiler-default LL/SC atomics. Consumers read/write `tail` to release
 * space. NC updates use bus-domain fences; same-node CC updates use CPU fences
 * in the plane-specific send/receive paths. */
typedef struct uct_obmm_fifo_ctl {
    /* 1st cacheline: producer-touched */
    volatile uint64_t head;
    UCS_CACHELINE_PADDING(uint64_t);

    /* 2nd cacheline: consumer-touched */
    volatile uint64_t tail;
    UCS_CACHELINE_PADDING(uint64_t);
} UCS_V_ALIGNED(UCS_SYS_CACHE_LINE_SIZE) uct_obmm_fifo_ctl_t;

/* Use anonymous padding fields, following the UCS_CACHELINE_PADDING pattern,
 * to express wire-layout gaps without semantic reserved members. */
#define UCT_OBMM_FIFO_ELEM_PADDING(_size) \
    char UCS_PP_APPEND_UNIQUE_ID(pad)[_size]


/* FIFO element header. am_short data starts at `header` (byte 16). am_bcopy
 * starts at byte 64 of the same element
 * because NC large bcopy fragments are very sensitive to that alignment.
 *
 * The two data ranges overlap intentionally: flags select exactly one payload
 * interpretation for each published FIFO element.
 */
typedef struct uct_obmm_fifo_element {
    uint8_t  flags;       /* UCT_OBMM_FIFO_ELEM_FLAG_xx */
    uint8_t  am_id;       /* active message id */
    UCT_OBMM_FIFO_ELEM_PADDING(2);
    uint32_t length;      /* bcopy payload bytes, or am_short [hdr|payload]
                             bytes in FIFO elements */
    UCT_OBMM_FIFO_ELEM_PADDING(8);
    uint64_t header;      /* am_short header; unused for bcopy */
    /* payload[length] follows here */
} UCS_S_PACKED uct_obmm_fifo_element_t;

#undef UCT_OBMM_FIFO_ELEM_PADDING


/* Compute slot stride: control header + fifo_size * elem_size, cacheline
 * aligned so that adjacent slots don't share a line. bcopy_seg_size is kept
 * in the signature because it remains part of the peer-visible geometry and
 * max_bcopy cap, but bcopy data now reuses the FIFO element data area.
 * Returned as size_t; callers must validate the result fits in the uint32_t
 * pool_hdr->slot_size field before passing to pool_attach. */
static UCS_F_ALWAYS_INLINE size_t
uct_obmm_slot_stride(unsigned fifo_size, unsigned fifo_elem_size,
                     unsigned bcopy_seg_size)
{
    (void)bcopy_seg_size;
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


static UCS_F_ALWAYS_INLINE unsigned
uct_obmm_fifo_short_data_offset(void)
{
    UCS_STATIC_ASSERT(ucs_offsetof(uct_obmm_fifo_element_t, length) == 4u);
    UCS_STATIC_ASSERT(ucs_offsetof(uct_obmm_fifo_element_t, header) ==
                      UCT_OBMM_FIFO_SHORT_DATA_OFFSET);
    UCS_STATIC_ASSERT(sizeof(uct_obmm_fifo_element_t) == 24u);
    return UCT_OBMM_FIFO_SHORT_DATA_OFFSET;
}


static UCS_F_ALWAYS_INLINE unsigned
uct_obmm_fifo_bcopy_data_offset(void)
{
    UCS_STATIC_ASSERT(sizeof(uct_obmm_fifo_element_t) <=
                      UCT_OBMM_FIFO_BCOPY_DATA_OFFSET);
    UCS_STATIC_ASSERT((UCT_OBMM_FIFO_BCOPY_DATA_OFFSET % 64u) == 0);
    return UCT_OBMM_FIFO_BCOPY_DATA_OFFSET;
}


static UCS_F_ALWAYS_INLINE unsigned
uct_obmm_fifo_max_short(unsigned fifo_elem_size)
{
    unsigned capacity = fifo_elem_size - uct_obmm_fifo_short_data_offset();

    return (capacity < UCT_OBMM_FIFO_MAX_SHORT) ? capacity :
                                                    UCT_OBMM_FIFO_MAX_SHORT;
}


static UCS_F_ALWAYS_INLINE unsigned
uct_obmm_fifo_max_bcopy(unsigned fifo_elem_size)
{
    return fifo_elem_size - uct_obmm_fifo_bcopy_data_offset();
}


static UCS_F_ALWAYS_INLINE uct_obmm_fifo_element_t*
uct_obmm_slot_elem(void *elems, uint64_t index, unsigned mask,
                   unsigned elem_size)
{
    return (uct_obmm_fifo_element_t*)
           UCS_PTR_BYTE_OFFSET(elems, (size_t)(index & mask) * elem_size);
}


static UCS_F_ALWAYS_INLINE void*
uct_obmm_fifo_elem_short_data(uct_obmm_fifo_element_t *elem)
{
    return UCS_PTR_BYTE_OFFSET(elem, uct_obmm_fifo_short_data_offset());
}


static UCS_F_ALWAYS_INLINE void*
uct_obmm_fifo_elem_bcopy_data(uct_obmm_fifo_element_t *elem)
{
    return UCS_PTR_BYTE_OFFSET(elem, uct_obmm_fifo_bcopy_data_offset());
}


/* Full bus-domain fence: orders ALL prior memory accesses (loads and
 * stores) before ALL subsequent memory accesses, in the outer-shareable /
 * device domain that includes cross-host obmm peers.
 *
 * Why we need this in addition to ucs_memory_bus_{store,load}_fence:
 * the receiver must order its payload LOADS (issued during the AM
 * handler) before the STORE that publishes the new tail. On ARM64,
 * ucs_memory_bus_store_fence() is `dmb oshst` (store→store only) and
 * does not order prior loads. Without this load→store barrier, a sender
 * could observe the advanced tail and overwrite the FIFO entry while the
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
