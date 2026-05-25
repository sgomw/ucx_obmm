/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "obmm_pool.h"
#include "obmm_atomic.h"

#include <ucs/arch/cpu.h>
#include <ucs/debug/log.h>
#include <ucs/sys/sys.h>
#include <ucs/sys/math.h>
#include <ucs/sys/ptr_arith.h>

#include <string.h>
#include <unistd.h>


#define UCT_OBMM_POOL_INIT_SPIN_LIMIT  (1u << 22) /* ~ a few seconds of spin */
#define UCT_OBMM_POOL_CLAIM_QUIESCE_SPIN_LIMIT  (1u << 16)


static UCS_F_ALWAYS_INLINE size_t
uct_obmm_pool_bitmap_words(uint32_t slot_count)
{
    return (slot_count + 63u) / 64u;
}


static UCS_F_ALWAYS_INLINE int
uct_obmm_pool_is_ready(uct_obmm_pool_t *pool)
{
    uint32_t state = pool->hdr->state;

    ucs_memory_bus_load_fence();
    return state == UCT_OBMM_POOL_STATE_READY;
}


static UCS_F_ALWAYS_INLINE void
uct_obmm_pool_clear_bit(volatile uint64_t *word, uint64_t bit)
{
    uint64_t cur, oldval;

    do {
        cur    = *word;
        oldval = uct_obmm_atomic_cswap64(word, cur, cur & ~bit);
    } while (oldval != cur);
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


static UCS_F_ALWAYS_INLINE int
uct_obmm_pool_initializer_stamped(uct_obmm_pool_hdr_t *hdr)
{
    return (hdr->initializer_pid != 0) && (hdr->initializer_starttime != 0);
}


/* CAS-claim the INITING transition; if we lose, wait until READY (or
 * recover the slot if the prior initializer died mid-init). */
static ucs_status_t
uct_obmm_pool_init_or_wait(uct_obmm_pool_hdr_t *hdr, uint32_t slot_count,
                           uint32_t slot_size)
{
    unsigned long      self_starttime = ucs_sys_get_proc_create_time(getpid());
    uint32_t           prev;
    unsigned           spin;
    size_t             bitmap_words = uct_obmm_pool_bitmap_words(slot_count);
    size_t             slot_off     = uct_obmm_pool_slot_offset(slot_count);

retry:
    prev = uct_obmm_atomic_cswap32(&hdr->state, UCT_OBMM_POOL_STATE_UNINIT,
                                   UCT_OBMM_POOL_STATE_INITING);

    if (prev == UCT_OBMM_POOL_STATE_UNINIT) {
        /* We won: zero metadata, fill geometry, publish READY */
        memset((char*)hdr + sizeof(*hdr), 0,
               bitmap_words * sizeof(uint64_t) +
               (size_t)slot_count * sizeof(uct_obmm_slot_meta_t));

        hdr->magic                 = UCT_OBMM_POOL_MAGIC;
        hdr->slot_count            = slot_count;
        hdr->slot_size             = slot_size;
        hdr->slot_array_offset     = slot_off;
        hdr->bitmap_words          = (uint32_t)bitmap_words;
        hdr->initializer_pid       = (uint32_t)getpid();
        hdr->initializer_starttime = (uint64_t)self_starttime;

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
            if (uct_obmm_pool_initializer_stamped(hdr) &&
                !uct_obmm_proc_alive(hdr->initializer_pid,
                                     hdr->initializer_starttime)) {
                ucs_warn("obmm: pool initializer pid=%u died; resetting "
                         "pool init state",
                         hdr->initializer_pid);
                uct_obmm_atomic_cswap32(&hdr->state,
                                        UCT_OBMM_POOL_STATE_INITING,
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
                                  uct_obmm_pool_t *pool)
{
    uct_obmm_pool_hdr_t *hdr = (uct_obmm_pool_hdr_t*)region_base;
    size_t               required;
    ucs_status_t         status;

    if ((slot_count == 0) || (slot_size == 0)) {
        return UCS_ERR_INVALID_PARAM;
    }

    required = uct_obmm_pool_required_size(slot_count, slot_size);
    if (region_size < required) {
        ucs_error("obmm: region size %zu < required pool size %zu "
                  "(slots=%u, slot_size=%u)",
                  region_size, required, slot_count, slot_size);
        return UCS_ERR_BUFFER_TOO_SMALL;
    }

    status = uct_obmm_pool_init_or_wait(hdr, slot_count, slot_size);
    if (status != UCS_OK) {
        return status;
    }

    if (hdr->magic != UCT_OBMM_POOL_MAGIC) {
        ucs_error("obmm: pool magic mismatch (got 0x%lx, expected 0x%lx)",
                  (unsigned long)hdr->magic, (unsigned long)UCT_OBMM_POOL_MAGIC);
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
    return UCS_OK;
}


/* Try to claim slot `idx` for the current process. Returns 1 on success
 * (bit was set by us, meta updated). Slots transition through CLAIMING so a
 * second process cannot take over while allocation/free is still publishing
 * ownership metadata. Dead owners (including crashed CLAIMING/DEAD states) may
 * be scavenged. Returns 0 if the slot is currently unclaimable. */
static int uct_obmm_pool_try_claim(uct_obmm_pool_t *pool, uint32_t idx,
                                   uint32_t self_pid, uint64_t self_starttime,
                                   uint32_t *generation_p)
{
    volatile uint64_t    *word    = &pool->bitmap[idx >> 6];
    uint64_t              bit     = 1ull << (idx & 63u);
    uct_obmm_slot_meta_t *m       = &pool->meta[idx];
    uint64_t              cur, oldval;
    uint32_t              state, rollback_state, state_prev, owner_pid;
    uint64_t              owner_starttime;
    int                   take_over = 0;

    if (!uct_obmm_pool_is_ready(pool)) {
        return 0;
    }

    cur = *word;
    ucs_memory_bus_load_fence();
    state           = m->state;
    rollback_state  = state;
    owner_pid       = m->owner_pid;
    owner_starttime = m->owner_starttime;

    if (!(cur & bit)) {
        /* A crashed claimer may leave CLAIMING behind with bit still clear.
         * Reset it to FREE once its owner is known dead, then retry normal
         * allocation from the FREE state. */
        if (state == UCT_OBMM_SLOT_STATE_CLAIMING) {
            if ((owner_pid == 0) ||
                uct_obmm_proc_alive(owner_pid, owner_starttime)) {
                return 0;
            }

            state_prev = uct_obmm_atomic_cswap32(&m->state,
                                                 UCT_OBMM_SLOT_STATE_CLAIMING,
                                                 UCT_OBMM_SLOT_STATE_FREE);
            if (state_prev != UCT_OBMM_SLOT_STATE_CLAIMING) {
                return 0;
            }
            state = UCT_OBMM_SLOT_STATE_FREE;
        }

        if (state != UCT_OBMM_SLOT_STATE_FREE) {
            return 0;
        }

        state_prev = uct_obmm_atomic_cswap32(&m->state,
                                             UCT_OBMM_SLOT_STATE_FREE,
                                             UCT_OBMM_SLOT_STATE_CLAIMING);
        if (state_prev != UCT_OBMM_SLOT_STATE_FREE) {
            return 0;
        }

        m->owner_pid       = self_pid;
        m->owner_starttime = self_starttime;
        ucs_memory_bus_store_fence();

        /* free: reserve the bitmap bit only after state moved to CLAIMING */
        oldval = uct_obmm_atomic_cswap64(word, cur, cur | bit);
        if (oldval != cur) {
            m->owner_pid       = 0;
            m->owner_starttime = 0;
            ucs_memory_bus_store_fence();
            m->state = UCT_OBMM_SLOT_STATE_FREE;
            ucs_memory_bus_store_fence();
            return 0; /* lost the race; caller will retry next slot */
        }
    } else {
        switch (state) {
        case UCT_OBMM_SLOT_STATE_CLAIMING:
            if ((owner_pid == 0) ||
                uct_obmm_proc_alive(owner_pid, owner_starttime)) {
                return 0;
            }

            state_prev = uct_obmm_atomic_cswap32(&m->state,
                                                 UCT_OBMM_SLOT_STATE_CLAIMING,
                                                 UCT_OBMM_SLOT_STATE_DEAD);
            if (state_prev != UCT_OBMM_SLOT_STATE_CLAIMING) {
                return 0;
            }
            state = UCT_OBMM_SLOT_STATE_DEAD;
            /* fall through */
        case UCT_OBMM_SLOT_STATE_DEAD:
            if ((owner_pid != 0) &&
                uct_obmm_proc_alive(owner_pid, owner_starttime)) {
                return 0;
            }

            state_prev = uct_obmm_atomic_cswap32(&m->state,
                                                 UCT_OBMM_SLOT_STATE_DEAD,
                                                 UCT_OBMM_SLOT_STATE_CLAIMING);
            if (state_prev != UCT_OBMM_SLOT_STATE_DEAD) {
                return 0;
            }
            take_over = 1;
            break;
        case UCT_OBMM_SLOT_STATE_IN_USE:
            if ((owner_pid != 0) &&
                uct_obmm_proc_alive(owner_pid, owner_starttime)) {
                return 0;
            }

            state_prev = uct_obmm_atomic_cswap32(&m->state,
                                                 UCT_OBMM_SLOT_STATE_IN_USE,
                                                 UCT_OBMM_SLOT_STATE_CLAIMING);
            if (state_prev != UCT_OBMM_SLOT_STATE_IN_USE) {
                return 0;
            }
            take_over = 1;
            break;
        default:
            return 0;
        }
    }

    if (!uct_obmm_pool_is_ready(pool)) {
        if (take_over) {
            m->state = rollback_state;
            ucs_memory_bus_store_fence();
        } else {
            uct_obmm_pool_clear_bit(word, bit);
            m->owner_pid       = 0;
            m->owner_starttime = 0;
            ucs_memory_bus_store_fence();
            m->state = UCT_OBMM_SLOT_STATE_FREE;
            ucs_memory_bus_store_fence();
        }
        return 0;
    }

    if (take_over) {
        m->owner_pid       = self_pid;
        m->owner_starttime = self_starttime;
        ucs_memory_bus_store_fence();
    }

    /* Bump generation so any in-flight stale messages from prior owner are
     * dropped by the receive path. memset slot bytes only AFTER bumping
     * generation, so any racing writer's bytes won't survive un-stamped. */
    m->generation += 1;
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
        if (!uct_obmm_pool_is_ready(pool)) {
            return UCS_ERR_NO_RESOURCE;
        }
        if (uct_obmm_pool_try_claim(pool, i, self_pid,
                                    (uint64_t)self_starttime, generation_p)) {
            *slot_index_p = i;
            *slot_ptr_p   = uct_obmm_pool_slot_ptr(pool, i);
            return UCS_OK;
        }
    }

    return UCS_ERR_NO_RESOURCE;
}


static int uct_obmm_pool_all_slots_free(uct_obmm_pool_t *pool)
{
    uint32_t i;

    for (i = 0; i < pool->hdr->bitmap_words; ++i) {
        if (pool->bitmap[i] != 0) {
            return 0;
        }
    }

    return 1;
}


static int uct_obmm_pool_has_live_slots(uct_obmm_pool_t *pool)
{
    uint32_t i;

    for (i = 0; i < pool->slot_count; ++i) {
        volatile uint64_t    *word = &pool->bitmap[i >> 6];
        uint64_t              bit  = 1ull << (i & 63u);
        uct_obmm_slot_meta_t *m    = &pool->meta[i];
        uint64_t              bits;
        uint32_t              state, owner_pid;
        uint64_t              owner_starttime;

        bits = *word;
        ucs_memory_bus_load_fence();

        state           = m->state;
        owner_pid       = m->owner_pid;
        owner_starttime = m->owner_starttime;

        if (!(bits & bit) && (state == UCT_OBMM_SLOT_STATE_FREE)) {
            continue;
        }

        if (uct_obmm_proc_alive(owner_pid, owner_starttime)) {
            return 1;
        }
    }

    return 0;
}


static int uct_obmm_pool_has_claiming_slots(uct_obmm_pool_t *pool)
{
    uint32_t i;

    for (i = 0; i < pool->slot_count; ++i) {
        uct_obmm_slot_meta_t *m = &pool->meta[i];

        ucs_memory_bus_load_fence();
        if (m->state == UCT_OBMM_SLOT_STATE_CLAIMING) {
            return 1;
        }
    }

    return 0;
}


static void uct_obmm_pool_wait_claims_to_quiesce(uct_obmm_pool_t *pool)
{
    unsigned spin;

    for (spin = 0; spin < UCT_OBMM_POOL_CLAIM_QUIESCE_SPIN_LIMIT; ++spin) {
        if (!uct_obmm_pool_has_claiming_slots(pool)) {
            return;
        }
    }
}


static void uct_obmm_pool_reclaim_stale_slots(uct_obmm_pool_t *pool)
{
    uint32_t i;

    for (i = 0; i < pool->slot_count; ++i) {
        volatile uint64_t    *word = &pool->bitmap[i >> 6];
        uint64_t              bit  = 1ull << (i & 63u);
        uct_obmm_slot_meta_t *m    = &pool->meta[i];
        uint64_t              bits;
        uint32_t              state, owner_pid;
        uint64_t              owner_starttime;

        bits = *word;
        ucs_memory_bus_load_fence();

        state           = m->state;
        owner_pid       = m->owner_pid;
        owner_starttime = m->owner_starttime;

        if (!(bits & bit) && (state == UCT_OBMM_SLOT_STATE_FREE)) {
            continue;
        }

        if (uct_obmm_proc_alive(owner_pid, owner_starttime)) {
            continue;
        }

        if (bits & bit) {
            uct_obmm_pool_clear_bit(word, bit);
        }

        if (state != UCT_OBMM_SLOT_STATE_FREE) {
            m->state = UCT_OBMM_SLOT_STATE_FREE;
            ucs_memory_bus_store_fence();
        }
    }
}


int uct_obmm_pool_free_slot(uct_obmm_pool_t *pool, uint32_t slot_index)
{
    volatile uint64_t    *word = &pool->bitmap[slot_index >> 6];
    uint64_t              bit  = 1ull << (slot_index & 63u);
    uct_obmm_slot_meta_t *m    = &pool->meta[slot_index];
    unsigned long         self_starttime;
    uint32_t              state_prev;

    /* Bump generation first so any in-flight stale messages are discarded by
     * a future re-using owner; mark DEAD and bus fence before clearing the
     * bit so a racing claimer sees the new generation. */
    m->generation += 1;
    m->state       = UCT_OBMM_SLOT_STATE_DEAD;
    ucs_memory_bus_store_fence();

    uct_obmm_pool_clear_bit(word, bit);

    m->owner_pid       = 0;
    m->owner_starttime = 0;
    m->state = UCT_OBMM_SLOT_STATE_FREE;
    ucs_memory_bus_store_fence();

    if (!uct_obmm_pool_all_slots_free(pool) &&
        uct_obmm_pool_has_live_slots(pool)) {
        return 0;
    }

    state_prev = uct_obmm_atomic_cswap32(&pool->hdr->state,
                                         UCT_OBMM_POOL_STATE_READY,
                                         UCT_OBMM_POOL_STATE_INITING);
    if (state_prev != UCT_OBMM_POOL_STATE_READY) {
        return 0;
    }

    self_starttime = ucs_sys_get_proc_create_time(getpid());
    pool->hdr->initializer_pid       = (uint32_t)getpid();
    pool->hdr->initializer_starttime = (uint64_t)self_starttime;
    ucs_memory_bus_store_fence();

    /* Once INITING is visible, no new claims start. Let any in-flight
     * alloc/takeover attempt finish its rollback before we touch stale slot
     * state, otherwise teardown could race a loser writing CLAIMING->oldstate. */
    uct_obmm_pool_wait_claims_to_quiesce(pool);

    /* Crash leftovers can keep stale bits set indefinitely. Before deciding
     * whether the export region may be reset to zero, reclaim any slot whose
     * recorded owner is already dead. */
    uct_obmm_pool_reclaim_stale_slots(pool);
    ucs_memory_bus_load_fence();
    if (uct_obmm_pool_has_live_slots(pool) ||
        !uct_obmm_pool_all_slots_free(pool)) {
        ucs_memory_bus_store_fence();
        pool->hdr->state = UCT_OBMM_POOL_STATE_READY;
        return 0;
    }

    return 1;
}


void uct_obmm_pool_reset(uct_obmm_pool_t *pool)
{
    uct_obmm_pool_hdr_t *hdr;
    char                *base;
    size_t               length;
    size_t               state_offset;
    size_t               state_end;
    size_t               init_pid_offset;
    size_t               init_start_offset;
    size_t               init_end;

    if ((pool == NULL) || (pool->hdr == NULL) || (pool->base == NULL)) {
        return;
    }

    hdr = pool->hdr;
    base   = (char*)pool->base;
    length = pool->length;

    state_offset      = offsetof(uct_obmm_pool_hdr_t, state);
    state_end         = state_offset + sizeof(hdr->state);
    init_pid_offset   = offsetof(uct_obmm_pool_hdr_t, initializer_pid);
    init_start_offset = offsetof(uct_obmm_pool_hdr_t, initializer_starttime);
    init_end          = init_start_offset + sizeof(hdr->initializer_starttime);

    if (state_offset > 0) {
        memset(base, 0, state_offset);
    }
    if (init_pid_offset > state_end) {
        memset(base + state_end, 0, init_pid_offset - state_end);
    }
    if (length > init_end) {
        memset(base + init_end, 0, length - init_end);
    }
    ucs_memory_bus_store_fence();

    hdr->initializer_pid       = 0;
    hdr->initializer_starttime = 0;
    ucs_memory_bus_store_fence();

    hdr->state = UCT_OBMM_POOL_STATE_UNINIT;
    ucs_memory_bus_store_fence();

    memset(pool, 0, sizeof(*pool));
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
    return UCS_OK;
}
