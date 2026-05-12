/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "obmm_pool.h"

#include <ucs/arch/atomic.h>
#include <ucs/arch/cpu.h>
#include <ucs/debug/log.h>
#include <ucs/sys/sys.h>
#include <ucs/sys/math.h>
#include <ucs/sys/ptr_arith.h>

#include <string.h>
#include <unistd.h>


#define UCT_OBMM_POOL_INIT_SPIN_LIMIT  (1u << 22) /* ~ a few seconds of spin */


static UCS_F_ALWAYS_INLINE size_t
uct_obmm_pool_bitmap_words(uint32_t slot_count)
{
    return (slot_count + 63u) / 64u;
}


static size_t uct_obmm_pool_meta_offset(uint32_t slot_count)
{
    return sizeof(uct_obmm_pool_hdr_t) +
           uct_obmm_pool_bitmap_words(slot_count) * sizeof(uint64_t);
}


static size_t uct_obmm_pool_slot_offset(uint32_t slot_count)
{
    return ucs_align_up(uct_obmm_pool_meta_offset(slot_count) +
                        (size_t)slot_count * sizeof(uct_obmm_slot_meta_t),
                        UCS_SYS_CACHE_LINE_SIZE);
}


size_t uct_obmm_pool_required_size(uint32_t slot_count, uint32_t slot_size)
{
    return uct_obmm_pool_slot_offset(slot_count) +
           (size_t)slot_count * slot_size;
}


static int uct_obmm_proc_alive(uint32_t pid, uint64_t starttime)
{
    unsigned long live;

    if (pid == 0) {
        return 0;
    }

    live = ucs_sys_get_proc_create_time((pid_t)pid);
    if (live == 0ul) {
        /* /proc/<pid>/stat unreadable: pid does not exist any more */
        return 0;
    }

    return (uint64_t)live == starttime;
}


/* CAS-claim the INITING transition; if we lose, wait until READY (or
 * recover the slot if the prior initializer died mid-init). */
static ucs_status_t
uct_obmm_pool_init_or_wait(uct_obmm_pool_hdr_t *hdr, uint32_t slot_count,
                           uint32_t slot_size, uint32_t version,
                           uct_obmm_mem_mode_t mode, uint32_t cc_chunk_size,
                           uint32_t cc_chunks_per_slot)
{
    unsigned long      self_starttime = ucs_sys_get_proc_create_time(getpid());
    uint32_t           prev;
    unsigned           spin;
    size_t             bitmap_words = uct_obmm_pool_bitmap_words(slot_count);
    size_t             slot_off     = uct_obmm_pool_slot_offset(slot_count);

retry:
    prev = ucs_atomic_cswap32(&hdr->state, UCT_OBMM_POOL_STATE_UNINIT,
                              UCT_OBMM_POOL_STATE_INITING);

    if (prev == UCT_OBMM_POOL_STATE_UNINIT) {
        /* We won: zero metadata, fill geometry, publish READY */
        memset((char*)hdr + sizeof(*hdr), 0,
               bitmap_words * sizeof(uint64_t) +
               (size_t)slot_count * sizeof(uct_obmm_slot_meta_t));

        hdr->magic                 = UCT_OBMM_POOL_MAGIC;
        hdr->version               = version;
        hdr->slot_count            = slot_count;
        hdr->slot_size             = slot_size;
        hdr->slot_array_offset     = slot_off;
        hdr->bitmap_words          = (uint32_t)bitmap_words;
        hdr->initializer_pid       = (uint32_t)getpid();
        hdr->initializer_starttime = (uint64_t)self_starttime;
        hdr->mode                  = mode;
        hdr->cc_chunk_size         = cc_chunk_size;
        hdr->cc_chunks_per_slot    = cc_chunks_per_slot;

        ucs_memory_bus_store_fence();
        hdr->state = UCT_OBMM_POOL_STATE_READY;
        return UCS_OK;
    }

    if (prev == UCT_OBMM_POOL_STATE_READY) {
        ucs_memory_bus_load_fence();
        return UCS_OK;
    }

    /* state == INITING : wait or recover */
    for (spin = 0; spin < UCT_OBMM_POOL_INIT_SPIN_LIMIT; ++spin) {
        ucs_memory_bus_load_fence();
        if (hdr->state == UCT_OBMM_POOL_STATE_READY) {
            return UCS_OK;
        }
        if (hdr->state == UCT_OBMM_POOL_STATE_UNINIT) {
            /* Some other rescuer reset; race for init */
            goto retry;
        }

        if ((spin & 0xfffu) == 0xfffu) {
            /* Periodically check if the initializer is alive. If not,
             * try to reset back to UNINIT so someone can re-init. */
            if (!uct_obmm_proc_alive(hdr->initializer_pid,
                                     hdr->initializer_starttime)) {
                ucs_warn("obmm: pool initializer pid=%u died; resetting "
                         "pool init state",
                         hdr->initializer_pid);
                ucs_atomic_cswap32(&hdr->state, UCT_OBMM_POOL_STATE_INITING,
                                   UCT_OBMM_POOL_STATE_UNINIT);
                goto retry;
            }
        }
    }

    ucs_error("obmm: timed out waiting for pool init to complete "
              "(initializer pid=%u)", hdr->initializer_pid);
    return UCS_ERR_TIMED_OUT;
}


ucs_status_t uct_obmm_pool_attach(void *region_base, size_t region_size,
                                   uint32_t slot_count, uint32_t slot_size,
                                   uint32_t version, uct_obmm_mem_mode_t mode,
                                   uint32_t cc_chunk_size,
                                   uint32_t cc_chunks_per_slot,
                                   uct_obmm_pool_t *pool)
{
    uct_obmm_pool_hdr_t *hdr = (uct_obmm_pool_hdr_t*)region_base;
    size_t               required;
    ucs_status_t         status;

    if ((slot_count == 0) || (slot_size == 0)) {
        return UCS_ERR_INVALID_PARAM;
    }
    if ((version != UCT_OBMM_POOL_VERSION_NC) &&
        (version != UCT_OBMM_POOL_VERSION_HYBRID)) {
        return UCS_ERR_INVALID_PARAM;
    }
    if (((mode == UCT_OBMM_MEM_MODE_NC) &&
         (version != UCT_OBMM_POOL_VERSION_NC)) ||
        ((mode == UCT_OBMM_MEM_MODE_HYBRID) &&
         (version != UCT_OBMM_POOL_VERSION_HYBRID))) {
        return UCS_ERR_INVALID_PARAM;
    }

    required = uct_obmm_pool_required_size(slot_count, slot_size);
    if (region_size < required) {
        ucs_error("obmm: region size %zu < required pool size %zu "
                  "(slots=%u, slot_size=%u)",
                  region_size, required, slot_count, slot_size);
        return UCS_ERR_BUFFER_TOO_SMALL;
    }

    status = uct_obmm_pool_init_or_wait(hdr, slot_count, slot_size, version,
                                        mode, cc_chunk_size,
                                        cc_chunks_per_slot);
    if (status != UCS_OK) {
        return status;
    }

    if (hdr->magic != UCT_OBMM_POOL_MAGIC) {
        ucs_error("obmm: pool magic mismatch (got 0x%lx, expected 0x%lx)",
                  (unsigned long)hdr->magic, (unsigned long)UCT_OBMM_POOL_MAGIC);
        return UCS_ERR_INVALID_PARAM;
    }
    if (hdr->version != version) {
        ucs_error("obmm: pool version mismatch (got %u, expected %u)",
                  hdr->version, version);
        return UCS_ERR_UNSUPPORTED;
    }
    if ((hdr->mode != mode) || (hdr->cc_chunk_size != cc_chunk_size) ||
        (hdr->cc_chunks_per_slot != cc_chunks_per_slot)) {
        ucs_error("obmm: pool mode/chunk mismatch (have mode=%u chunk=%u/%u, "
                  "expected mode=%u chunk=%u/%u)",
                  hdr->mode, hdr->cc_chunk_size, hdr->cc_chunks_per_slot,
                  mode, cc_chunk_size, cc_chunks_per_slot);
        return UCS_ERR_INVALID_PARAM;
    }
    if ((hdr->slot_count != slot_count) || (hdr->slot_size != slot_size)) {
        ucs_error("obmm: pool geometry mismatch "
                  "(have slots=%u size=%u, expected %u/%u)",
                  hdr->slot_count, hdr->slot_size, slot_count, slot_size);
        return UCS_ERR_INVALID_PARAM;
    }

    pool->base       = region_base;
    pool->length     = region_size;
    pool->hdr        = hdr;
    pool->bitmap     = (volatile uint64_t*)((char*)hdr + sizeof(*hdr));
    pool->meta       = (uct_obmm_slot_meta_t*)((char*)hdr +
                                               uct_obmm_pool_meta_offset(slot_count));
    pool->slots      = (char*)hdr + hdr->slot_array_offset;
    pool->slot_count = slot_count;
    pool->slot_size  = slot_size;
    pool->version    = hdr->version;
    pool->mode       = (uct_obmm_mem_mode_t)hdr->mode;
    pool->cc_chunk_size      = hdr->cc_chunk_size;
    pool->cc_chunks_per_slot = hdr->cc_chunks_per_slot;
    return UCS_OK;
}


/* Try to claim slot `idx` for the current process. Returns 1 on success
 * (bit was set by us, meta updated). On scavenge of a dead owner, generation
 * is bumped and the slot is taken over. Returns 0 if the slot is unclaimable
 * (live owner). */
static int uct_obmm_pool_try_claim(uct_obmm_pool_t *pool, uint32_t idx,
                                   uint32_t self_pid, uint64_t self_starttime,
                                   uint32_t *generation_p)
{
    volatile uint64_t    *word    = &pool->bitmap[idx >> 6];
    uint64_t              bit     = 1ull << (idx & 63u);
    uct_obmm_slot_meta_t *m       = &pool->meta[idx];
    uint64_t              cur, oldval;
    int                   take_over;

    cur = *word;
    if (cur & bit) {
        /* allocated: maybe the owner is dead */
        ucs_memory_bus_load_fence();
        if ((m->state == UCT_OBMM_SLOT_STATE_IN_USE) &&
            uct_obmm_proc_alive(m->owner_pid, m->owner_starttime)) {
            return 0;
        }
        take_over = 1;
    } else {
        /* free: try to set the bit */
        oldval = ucs_atomic_cswap64(word, cur, cur | bit);
        if (oldval != cur) {
            return 0; /* lost the race; caller will retry next slot */
        }
        take_over = 0;
    }

    /* Bump generation so any in-flight stale messages from prior owner are
     * dropped by the receive path. memset slot bytes only AFTER bumping
     * generation, so any racing writer's bytes won't survive un-stamped. */
    m->generation += 1;
    m->owner_pid       = self_pid;
    m->owner_starttime = self_starttime;
    /* Zero the slot bytes for our use (FIFO ctl + elements). */
    memset(uct_obmm_pool_slot_ptr(pool, idx), 0, pool->slot_size);
    ucs_memory_bus_store_fence();
    m->state = UCT_OBMM_SLOT_STATE_IN_USE;
    ucs_memory_bus_store_fence();

    *generation_p = m->generation;
    (void)take_over;
    return 1;
}


ucs_status_t uct_obmm_pool_alloc_slot(uct_obmm_pool_t *pool,
                                      uint32_t *slot_index_p,
                                      void **slot_ptr_p,
                                      uint32_t *generation_p)
{
    uint32_t      self_pid       = (uint32_t)getpid();
    unsigned long self_starttime = ucs_sys_get_proc_create_time(getpid());
    uint32_t      i;

    if (self_starttime == 0ul) {
        ucs_warn("obmm: cannot read self process start time; "
                 "scavenge will treat us as dead");
    }

    for (i = 0; i < pool->slot_count; ++i) {
        if (uct_obmm_pool_try_claim(pool, i, self_pid,
                                    (uint64_t)self_starttime, generation_p)) {
            *slot_index_p = i;
            *slot_ptr_p   = uct_obmm_pool_slot_ptr(pool, i);
            return UCS_OK;
        }
    }

    return UCS_ERR_NO_RESOURCE;
}


void uct_obmm_pool_free_slot(uct_obmm_pool_t *pool, uint32_t slot_index)
{
    volatile uint64_t    *word = &pool->bitmap[slot_index >> 6];
    uint64_t              bit  = 1ull << (slot_index & 63u);
    uct_obmm_slot_meta_t *m    = &pool->meta[slot_index];
    uint64_t              cur, oldval;

    /* Bump generation first so any in-flight stale messages are discarded by
     * a future re-using owner; mark DEAD and bus fence before clearing the
     * bit so a racing claimer sees the new generation. */
    m->generation += 1;
    m->state       = UCT_OBMM_SLOT_STATE_DEAD;
    m->owner_pid   = 0;
    m->owner_starttime = 0;
    ucs_memory_bus_store_fence();

    do {
        cur    = *word;
        oldval = ucs_atomic_cswap64(word, cur, cur & ~bit);
    } while (oldval != cur);

    m->state = UCT_OBMM_SLOT_STATE_FREE;
    ucs_memory_bus_store_fence();
}


ucs_status_t uct_obmm_pool_open(void *region_base, size_t region_size,
                                uct_obmm_pool_t *pool)
{
    uct_obmm_pool_hdr_t *hdr = (uct_obmm_pool_hdr_t*)region_base;
    uint32_t             state;
    uint32_t             slot_count, slot_size;
    size_t               required;

    if (region_size < sizeof(*hdr)) {
        return UCS_ERR_BUFFER_TOO_SMALL;
    }

    state = hdr->state;
    ucs_memory_bus_load_fence();
    if (state != UCT_OBMM_POOL_STATE_READY) {
        ucs_debug("obmm: pool at %p not READY (state=%u)", region_base, state);
        return UCS_ERR_NO_RESOURCE;
    }

    if (hdr->magic != UCT_OBMM_POOL_MAGIC) {
        ucs_error("obmm: pool magic mismatch on open (got 0x%lx)",
                  (unsigned long)hdr->magic);
        return UCS_ERR_INVALID_PARAM;
    }
    if ((hdr->version != UCT_OBMM_POOL_VERSION_NC) &&
        (hdr->version != UCT_OBMM_POOL_VERSION_HYBRID)) {
        ucs_error("obmm: pool version mismatch on open (got %u)", hdr->version);
        return UCS_ERR_UNSUPPORTED;
    }

    slot_count = hdr->slot_count;
    slot_size  = hdr->slot_size;
    if ((slot_count == 0) || (slot_size == 0)) {
        return UCS_ERR_INVALID_PARAM;
    }

    required = uct_obmm_pool_required_size(slot_count, slot_size);
    if (region_size < required) {
        ucs_error("obmm: peer pool geometry exceeds region size "
                  "(slots=%u, slot_size=%u, region=%zu, required=%zu)",
                  slot_count, slot_size, region_size, required);
        return UCS_ERR_INVALID_PARAM;
    }

    pool->base       = region_base;
    pool->length     = region_size;
    pool->hdr        = hdr;
    pool->bitmap     = (volatile uint64_t*)((char*)hdr + sizeof(*hdr));
    pool->meta       = (uct_obmm_slot_meta_t*)((char*)hdr +
                                               uct_obmm_pool_meta_offset(slot_count));
    pool->slots      = (char*)hdr + hdr->slot_array_offset;
    pool->slot_count = slot_count;
    pool->slot_size  = slot_size;
    pool->version    = hdr->version;
    pool->mode       = (uct_obmm_mem_mode_t)hdr->mode;
    pool->cc_chunk_size      = hdr->cc_chunk_size;
    pool->cc_chunks_per_slot = hdr->cc_chunks_per_slot;
    return UCS_OK;
}
