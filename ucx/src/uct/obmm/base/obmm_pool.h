/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_POOL_H_
#define UCT_OBMM_POOL_H_

#include <ucs/sys/compiler_def.h>
#include <ucs/type/status.h>

#include <stddef.h>
#include <stdint.h>


#define UCT_OBMM_POOL_MAGIC    0x4f424d50554c5333ull /* "OBMPULS3" */


enum {
    UCT_OBMM_POOL_STATE_UNINIT = 0u,
    UCT_OBMM_POOL_STATE_INITING = 1u,
    UCT_OBMM_POOL_STATE_READY = 2u
};


enum {
    UCT_OBMM_SLOT_STATE_FREE = 0u,
    UCT_OBMM_SLOT_STATE_IN_USE = 1u,
    UCT_OBMM_SLOT_STATE_DEAD = 2u,
    UCT_OBMM_SLOT_STATE_CLAIMING = 3u
};


/* In-region pool header. Lives at offset 0 of the local export region. All
 * fields are written via non-cacheable mappings: state transitions must be
 * paired with ucs_memory_bus_store_fence so peers (potentially on another
 * host) see the geometry before the READY transition. During cleanup, INITING
 * is only a transient coordination state; once reset completes, the shared
 * region is fully zero again. */
typedef struct uct_obmm_pool_hdr {
    uint64_t magic;             /* UCT_OBMM_POOL_MAGIC */
    uint32_t state;             /* UCT_OBMM_POOL_STATE_xx, atomic */
    uint32_t slot_count;
    uint32_t slot_size;
    uint64_t slot_array_offset; /* offset (from region base) of slot 0 */
    uint32_t bitmap_words;      /* number of u64 words in alloc bitmap */
    uint32_t initializer_pid;   /* pid that owned the INITING transition */
    uint64_t initializer_starttime;
    /* followed by:
     *   uint64_t alloc_bitmap[bitmap_words];
     *   uct_obmm_slot_meta_t slot_meta[slot_count];
     */
} uct_obmm_pool_hdr_t;


/* Per-slot ownership metadata. Stored adjacent to the bitmap, separate from
 * the slot's actual FIFO contents. */
typedef struct uct_obmm_slot_meta {
    uint32_t state;            /* UCT_OBMM_SLOT_STATE_xx */
    uint32_t generation;       /* monotonically increasing; bumped on free,
                                  scavenge, and when readers must discard
                                  in-flight stale elements */
    uint32_t owner_pid;
    uint32_t reserved;
    uint64_t owner_starttime;  /* /proc/<pid>/stat field 22, for liveness */
} uct_obmm_slot_meta_t;


/* Convenience accessor populated by uct_obmm_pool_attach. Pointers point
 * into the mapped region; nothing is owned by this struct. */
typedef struct uct_obmm_pool {
    void                  *base;     /* region base (pool hdr) */
    size_t                 length;   /* mapped region size */
    uct_obmm_pool_hdr_t   *hdr;
    volatile uint64_t     *bitmap;
    uct_obmm_slot_meta_t  *meta;
    void                  *slots;    /* base + hdr->slot_array_offset */
    uint32_t               slot_count;
    uint32_t               slot_size;
} uct_obmm_pool_t;


/* Compute the minimum region size required to hold a pool with the given
 * geometry. */
size_t uct_obmm_pool_required_size(uint32_t slot_count, uint32_t slot_size);


/* Initialize the pool inside `region_base` if not already initialized, or
 * attach to an existing valid pool. Cross-process safe: races between
 * multiple processes attaching to the same region are resolved via the
 * hdr->state CAS. On success, `pool` is populated with cached pointers.
 *
 * If an existing pool's geometry doesn't match the requested
 * (slot_count, slot_size), returns UCS_ERR_INVALID_PARAM.
 */
ucs_status_t uct_obmm_pool_attach(void *region_base, size_t region_size,
                                  uint32_t slot_count, uint32_t slot_size,
                                  uct_obmm_pool_t *pool);


/* Allocate a free slot. Scavenges slots owned by dead processes. Returns the
 * slot index (0..slot_count-1), a pointer to the slot bytes (length
 * slot_size), and the generation token the caller must stamp into outgoing
 * iface_addr. Slot bytes are zeroed before return.
 *
 * Returns UCS_ERR_NO_RESOURCE if no slot can be obtained (all slots in use
 * by live processes).
 */
ucs_status_t uct_obmm_pool_alloc_slot(uct_obmm_pool_t *pool,
                                      uint32_t *slot_index_p,
                                      void **slot_ptr_p,
                                      uint32_t *generation_p);


/* Release a slot the caller owns. Bumps generation so any stale in-flight
 * elements written by remote senders are dropped on the next receiver's
 * dispatch. Marks slot as FREE in the bitmap. Returns non-zero only if this
 * caller released the final local slot and acquired exclusive permission to
 * reset the whole export region to zero before another attach re-initializes
 * it. */
int uct_obmm_pool_free_slot(uct_obmm_pool_t *pool, uint32_t slot_index);


/* Reset the whole local export pool region to zero. The caller must already
 * hold exclusive cleanup ownership from uct_obmm_pool_free_slot(); this helper
 * keeps the shared state in INITING until the rest of the region is zero so a
 * new attach cannot race against a partially cleared pool. The final published
 * shared image is literal all-zero memory. */
void uct_obmm_pool_reset(uct_obmm_pool_t *pool);


/* Compute pointer to a slot by index. Does not validate ownership. */
static UCS_F_ALWAYS_INLINE void*
uct_obmm_pool_slot_ptr(const uct_obmm_pool_t *pool, uint32_t slot_index)
{
    return (char*)pool->slots + (size_t)slot_index * pool->slot_size;
}


/* Read-only attach: validates that an existing READY pool lives at
 * `region_base`, and populates `pool` with cached pointers. Does NOT
 * attempt initialization. Used by ep_create to access a peer's pool
 * without owning it. Returns UCS_ERR_NO_RESOURCE if the pool is not
 * READY (e.g. peer iface was destroyed between address exchange and ep
 * create). */
ucs_status_t uct_obmm_pool_open(void *region_base, size_t region_size,
                                uct_obmm_pool_t *pool);

#endif
