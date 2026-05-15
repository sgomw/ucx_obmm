/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "obmm_ep.h"
#include "obmm_iface.h"
#include "obmm_pool.h"
#include "obmm_fifo.h"

#include <uct/base/uct_log.h>
#include <uct/base/uct_iface.h>
#include <ucs/arch/atomic.h>
#include <ucs/arch/cpu.h>
#include <ucs/debug/log.h>
#include <ucs/debug/memtrack_int.h>
#include <ucs/sys/math.h>
#include <ucs/sys/ptr_arith.h>
#include <ucs/sys/sys.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>


static UCS_F_ALWAYS_INLINE int
uct_obmm_ep_has_tx_resource(uct_obmm_ep_t *ep);

static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_reserve_slot(uct_obmm_ep_t *ep, uint64_t *head_p);


static int uct_obmm_diag_bcopy_enabled(void)
{
    const char *env = getenv("UCX_OBMM_DIAG_BCOPY");

    return (env != NULL) && (env[0] != '\0') &&
           ((env[0] != '0') || (env[1] != '\0'));
}


static UCS_F_ALWAYS_INLINE uint64_t
uct_obmm_ep_make_lock_token(uct_obmm_iface_t *iface, const uct_obmm_ep_t *ep)
{
    uint64_t token;

    token = iface->region->info.exporter_dcna ^
            iface->region->info.exporter_deid.hi ^
            iface->region->info.exporter_deid.lo ^
            ((uint64_t)iface->slot_index << 32) ^
            iface->generation ^
            (uint64_t)(uintptr_t)ep;
    return (token == 0) ? 1 : token;
}


static UCS_F_ALWAYS_INLINE int
uct_obmm_ep_try_lock_head(uct_obmm_ep_t *ep)
{
    ucs_memory_bus_load_fence();
    if (ep->peer_ctl->lock != 0) {
        return 0;
    }

    (void)ucs_atomic_cswap64(&ep->peer_ctl->lock, 0, ep->lock_token);
    uct_obmm_bus_full_fence();
    return ep->peer_ctl->lock == ep->lock_token;
}


static UCS_F_ALWAYS_INLINE void
uct_obmm_ep_unlock_head(uct_obmm_ep_t *ep)
{
    ucs_memory_bus_store_fence();
    ep->peer_ctl->lock = 0;
}


static UCS_CLASS_INIT_FUNC(uct_obmm_ep_t, const uct_ep_params_t *params)
{
    uct_obmm_iface_t             *iface = ucs_derived_of(params->iface,
                                                         uct_obmm_iface_t);
    uct_obmm_md_t                *md    = ucs_derived_of(iface->super.super.md,
                                                         uct_obmm_md_t);
    const uct_obmm_device_addr_t *daddr;
    const uct_obmm_iface_addr_t  *iaddr;
    uct_obmm_region_t            *region;
    uct_obmm_region_t            *cc_region;
    uct_obmm_pool_t               peer_pool;
    void                         *peer_slot;
    void                         *peer_cc_slice_base;
    size_t                        peer_cc_slice_size;
    ucs_status_t                  status;

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super.super);

    ucs_arbiter_group_init(&self->arb_group);

    daddr = (const uct_obmm_device_addr_t*)params->dev_addr;
    iaddr = (const uct_obmm_iface_addr_t*)params->iface_addr;

    /* Reject incompatible geometry. UCX wireup should already have filtered
     * this out via is_reachable_v2, but double-check. */
    if ((iaddr->fifo_size != iface->fifo_size) ||
        (iaddr->fifo_elem_size != iface->fifo_elem_size) ||
        (iaddr->bcopy_seg_size != iface->bcopy_seg_size) ||
        (iaddr->mode != iface->mode) ||
        (iaddr->pool_version != iface->pool_version)) {
        ucs_error("obmm: peer geometry/mode (mode=%u ver=%u fifo=%u elem=%u "
                  "seg=%u) differs from local (mode=%u ver=%u fifo=%u elem=%u "
                  "seg=%u); ep_create rejected",
                  iaddr->mode, iaddr->pool_version,
                  iaddr->fifo_size, iaddr->fifo_elem_size,
                  iaddr->bcopy_seg_size,
                  iface->mode, iface->pool_version,
                  iface->fifo_size, iface->fifo_elem_size,
                  iface->bcopy_seg_size);
        return UCS_ERR_UNREACHABLE;
    }

    if ((iface->mode == UCT_OBMM_MEM_MODE_HYBRID) &&
        ((iaddr->cc_chunk_size != iface->cc_chunk_size) ||
         (iaddr->cc_chunks_per_slot != iface->cc_chunks_per_slot) ||
         (iaddr->cc_total_chunks != iface->cc_total_chunks) ||
         (iaddr->cc_exporters_hash != iface->cc_exporters_hash))) {
        ucs_error("obmm: peer hybrid geometry/hash differs from local");
        return UCS_ERR_UNREACHABLE;
    }

    /* Find the local region (export-for-self, import-for-remote) that maps
     * the peer's exporter coordinates. */
    {
        uct_obmm_region_t *exp_r;
        uct_obmm_eid_t     eid;

        eid.hi = daddr->nc_exporter_deid_hi;
        eid.lo = daddr->nc_exporter_deid_lo;

        exp_r  = uct_obmm_md_export_region(md);
        region = NULL;
        if ((exp_r != NULL) &&
            (exp_r->info.exporter_dcna == daddr->nc_exporter_dcna) &&
            (exp_r->info.exporter_deid.hi == eid.hi) &&
            (exp_r->info.exporter_deid.lo == eid.lo)) {
            region = exp_r;
        } else {
            region = uct_obmm_md_find_import_region(md,
                                                    daddr->nc_exporter_dcna,
                                                    &eid);
        }
    }
    if (region == NULL) {
        ucs_error("obmm: ep_create cannot find region for peer "
                  "dcna=0x%lx deid=0x%lx:0x%lx",
                  (unsigned long)daddr->nc_exporter_dcna,
                  (unsigned long)daddr->nc_exporter_deid_hi,
                  (unsigned long)daddr->nc_exporter_deid_lo);
        return UCS_ERR_UNREACHABLE;
    }

    cc_region = NULL;
    if (iface->mode == UCT_OBMM_MEM_MODE_HYBRID) {
        if (iaddr->cc_exporter_index >= md->num_cc_exporters) {
            ucs_error("obmm: ep_create cannot find peer CC region/index");
            return UCS_ERR_UNREACHABLE;
        }
        cc_region = uct_obmm_md_find_cc_region_by_index(md,
                                                        iaddr->cc_exporter_index);
        if (cc_region == NULL) {
            ucs_error("obmm: ep_create cannot find peer CC region/index");
            return UCS_ERR_UNREACHABLE;
        }
    }

    status = uct_obmm_pool_open(region->base, region->length, &peer_pool);
    if (status != UCS_OK) {
        ucs_error("obmm: failed to open peer pool: %s",
                  ucs_status_string(status));
        return status;
    }

    if (iaddr->slot_index >= peer_pool.slot_count) {
        ucs_error("obmm: peer slot_index %u out of range (slot_count=%u)",
                  iaddr->slot_index, peer_pool.slot_count);
        return UCS_ERR_INVALID_PARAM;
    }
    if ((peer_pool.version != iface->pool_version) ||
        (peer_pool.mode != iface->mode)) {
        ucs_error("obmm: peer pool version/mode mismatch (peer %u/%u local "
                  "%u/%u)", peer_pool.version, peer_pool.mode,
                  iface->pool_version, iface->mode);
        return UCS_ERR_UNREACHABLE;
    }

    if (peer_pool.slot_size !=
        uct_obmm_slot_stride(iaddr->fifo_size, iaddr->fifo_elem_size,
                             iaddr->bcopy_seg_size)) {
        ucs_error("obmm: peer pool slot_size %u inconsistent with iface_addr "
                  "geometry (fifo=%u elem=%u seg=%u)",
                  peer_pool.slot_size, iaddr->fifo_size,
                  iaddr->fifo_elem_size, iaddr->bcopy_seg_size);
        return UCS_ERR_INVALID_PARAM;
    }

    peer_slot = uct_obmm_pool_slot_ptr(&peer_pool, iaddr->slot_index);

    self->peer_ctl            = uct_obmm_slot_ctl(peer_slot);
    self->peer_elems          = uct_obmm_slot_elems(peer_slot);
    self->peer_descs          = uct_obmm_slot_descs(peer_slot,
                                                    iaddr->fifo_size,
                                                    iaddr->fifo_elem_size);
    ucs_memory_bus_load_fence();
    self->cached_tail         = self->peer_ctl->tail;
    self->expected_generation = iaddr->generation;
    self->fifo_size           = iaddr->fifo_size;
    self->fifo_mask           = iaddr->fifo_size - 1u;
    self->fifo_elem_size      = iaddr->fifo_elem_size;
    self->bcopy_seg_size      = iaddr->bcopy_seg_size;
    self->peer_dcna           = daddr->nc_exporter_dcna;
    self->peer_deid_hi        = daddr->nc_exporter_deid_hi;
    self->peer_deid_lo        = daddr->nc_exporter_deid_lo;
    self->peer_slot_index     = iaddr->slot_index;
    self->peer_pid            = iaddr->pid;
    self->lock_token          = uct_obmm_ep_make_lock_token(iface, self);
    self->peer_cc_region      = cc_region;
    self->peer_cc_exporter_index = iaddr->cc_exporter_index;
    if (uct_obmm_diag_bcopy_enabled()) {
        fprintf(stderr, "obmmD E n=%c c=%c ps=%u pg=%u\n",
                (region->info.type == UCT_OBMM_DEV_IMPORT) ? 'I' : 'E',
                (cc_region == NULL) ? 'N' :
                ((cc_region->info.type == UCT_OBMM_DEV_IMPORT) ? 'I' : 'E'),
                iaddr->slot_index, iaddr->generation);
        fflush(stderr);
    }
    self->cc_inflight_capacity = (iface->mode == UCT_OBMM_MEM_MODE_HYBRID) ?
                                 iface->cc_chunks_per_slot : 0;
    self->cc_inflight_head     = 0;
    self->cc_inflight_count    = 0;
    self->cc_inflight          = NULL;
    if (iface->mode == UCT_OBMM_MEM_MODE_HYBRID) {
        self->cc_inflight = ucs_calloc(self->cc_inflight_capacity,
                                       sizeof(*self->cc_inflight),
                                       "obmm_cc_inflight");
        if (self->cc_inflight == NULL) {
            return UCS_ERR_NO_MEMORY;
        }
        if (uct_obmm_diag_cc_handoff_enabled()) {
            peer_cc_slice_size = iface->cc_chunks_per_slot * iface->cc_chunk_size;
            peer_cc_slice_base = UCS_PTR_BYTE_OFFSET(cc_region->base,
                                                     (size_t)iaddr->slot_index *
                                                     peer_cc_slice_size);
            status = uct_obmm_region_set_ownership(cc_region, peer_cc_slice_base,
                                                   peer_cc_slice_size,
                                                   PROT_NONE);
            if (status != UCS_OK) {
                ucs_free(self->cc_inflight);
                self->cc_inflight = NULL;
                return status;
            }
        }
        ucs_list_add_tail(&iface->eps, &self->list);
    }
    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_obmm_ep_t)
{
    /* Drain any UCP requests still parked on this ep's arbiter group
     * before the iface tears down its arbiter. mm follows the same
     * order (mm_ep.c:217). */
    uct_obmm_ep_pending_purge(&self->super.super, NULL, NULL);
    if (self->cc_inflight != NULL) {
        uct_obmm_ep_reclaim_chunks(self);
        ucs_list_del(&self->list);
        ucs_free(self->cc_inflight);
    }
    /* Peer pool memory is owned by the MD; nothing else to release. */
}

UCS_CLASS_DEFINE(uct_obmm_ep_t, uct_base_ep_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_obmm_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_obmm_ep_t, uct_ep_t);


int uct_obmm_ep_is_connected(const uct_ep_h tl_ep,
                             const uct_ep_is_connected_params_t *params)
{
    const uct_obmm_ep_t          *ep = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    const uct_obmm_device_addr_t *daddr;
    const uct_obmm_iface_addr_t  *iaddr;

    if (!uct_base_ep_is_connected(tl_ep, params)) {
        return 0;
    }

    daddr = (const uct_obmm_device_addr_t*)params->device_addr;
    iaddr = (const uct_obmm_iface_addr_t*)params->iface_addr;
    if ((daddr == NULL) || (iaddr == NULL)) {
        return 0;
    }

    return (daddr->nc_exporter_dcna == ep->peer_dcna) &&
           (daddr->nc_exporter_deid_hi == ep->peer_deid_hi) &&
           (daddr->nc_exporter_deid_lo == ep->peer_deid_lo) &&
           (iaddr->slot_index == ep->peer_slot_index) &&
           (iaddr->generation == ep->expected_generation);
}


ucs_status_t uct_obmm_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t header,
                                  const void *payload, unsigned length)
{
    uct_obmm_ep_t           *ep    = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    uct_obmm_iface_t        *iface = ucs_derived_of(tl_ep->iface,
                                                    uct_obmm_iface_t);
    size_t                   payload_total = sizeof(header) + length;
    uct_obmm_fifo_element_t *elem;
    uint64_t                 head;
    uint8_t                  owner_bit;
    ucs_status_t             status;

    UCT_CHECK_AM_ID(id);
    UCT_CHECK_LENGTH(payload_total, 0,
                     ep->fifo_elem_size - sizeof(uct_obmm_fifo_element_t),
                     "am_short");

    status = uct_obmm_ep_reserve_slot(ep, &head);
    if (status != UCS_OK) {
        return status;
    }

    elem = uct_obmm_slot_elem(ep->peer_elems, head, ep->fifo_mask,
                              ep->fifo_elem_size);

    elem->am_id      = id;
    elem->length     = (uint16_t)payload_total;
    elem->generation = ep->expected_generation;
    elem->header     = header;
    if (length > 0) {
        memcpy(elem + 1, payload, length);
    }

    /* Publish: the OWNER bit toggles each wraparound. The receiver expects
     * bit==1 on even passes and bit==0 on odd passes (and vice versa) so
     * that uninitialized memory (zero) reads as "not yet written" on the
     * first pass. */
    owner_bit = ((head / ep->fifo_size) & 1u) ? 0u : UCT_OBMM_FIFO_ELEM_FLAG_OWNER;

    ucs_memory_bus_store_fence();
    elem->flags = owner_bit;

    UCT_TL_EP_STAT_OP(&ep->super, AM, SHORT, payload_total);
    uct_iface_trace_am(&iface->super.super, UCT_AM_TRACE_TYPE_SEND, id,
                       &header, payload_total, "TX: AM_SHORT");
    return UCS_OK;
}


/* Reserve one slot in the peer's FIFO, returning the head index that was
 * claimed. Multi-producer reservation is serialized with a token lock because
 * NC/aarch64 cross-node CAS return values are not reliable ownership results;
 * success is determined by reading back our unique token. */
static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_reserve_slot(uct_obmm_ep_t *ep, uint64_t *head_p)
{
    uint64_t head;

    if (!uct_obmm_ep_try_lock_head(ep)) {
        UCS_STATS_UPDATE_COUNTER(ep->super.stats, UCT_EP_STAT_NO_RES, 1);
        return UCS_ERR_NO_RESOURCE;
    }

    ucs_memory_bus_load_fence();
    head = ep->peer_ctl->head;

    if ((head - ep->cached_tail) >= ep->fifo_size) {
        ucs_memory_bus_load_fence();
        ep->cached_tail = ep->peer_ctl->tail;
        if ((head - ep->cached_tail) >= ep->fifo_size) {
            UCS_STATS_UPDATE_COUNTER(ep->super.stats, UCT_EP_STAT_NO_RES, 1);
            uct_obmm_ep_unlock_head(ep);
            return UCS_ERR_NO_RESOURCE;
        }
    }

    uct_obmm_bus_full_fence();
    ep->peer_ctl->head = head + 1;
    uct_obmm_ep_unlock_head(ep);

    *head_p = head;
    return UCS_OK;
}


static UCS_F_ALWAYS_INLINE void
uct_obmm_iface_push_cc_chunk(uct_obmm_iface_t *iface, uint16_t chunk_index)
{
    ucs_assert(iface->cc_free_top < iface->cc_chunks_per_slot);
    iface->cc_free_stack[iface->cc_free_top++] = chunk_index;
}


static UCS_F_ALWAYS_INLINE void
uct_obmm_iface_requeue_cc_chunk(uct_obmm_iface_t *iface, uint16_t chunk_index)
{
    ucs_assert(iface->cc_free_top < iface->cc_chunks_per_slot);
    memmove(&iface->cc_free_stack[1], &iface->cc_free_stack[0],
            iface->cc_free_top * sizeof(*iface->cc_free_stack));
    iface->cc_free_stack[0] = chunk_index;
    ++iface->cc_free_top;
}


static UCS_F_ALWAYS_INLINE uint16_t
uct_obmm_iface_pop_cc_chunk(uct_obmm_iface_t *iface)
{
    ucs_assert(iface->cc_free_top > 0);
    return iface->cc_free_stack[--iface->cc_free_top];
}


static UCS_F_ALWAYS_INLINE void
uct_obmm_ep_push_inflight(uct_obmm_ep_t *ep, uint64_t fifo_head,
                          uint16_t chunk_index)
{
    unsigned index;

    ucs_assert(ep->cc_inflight_count < ep->cc_inflight_capacity);
    index = (ep->cc_inflight_head + ep->cc_inflight_count) %
            ep->cc_inflight_capacity;
    ep->cc_inflight[index].fifo_head   = fifo_head;
    ep->cc_inflight[index].chunk_index = chunk_index;
    ++ep->cc_inflight_count;
}


unsigned uct_obmm_ep_reclaim_chunks(uct_obmm_ep_t *ep)
{
    uct_obmm_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                             uct_obmm_iface_t);
    uct_obmm_cc_inflight_t *entry;
    uint64_t               peer_tail;
    unsigned               count = 0;

    if ((iface->mode != UCT_OBMM_MEM_MODE_HYBRID) ||
        (ep->cc_inflight_count == 0)) {
        return 0;
    }

    ucs_memory_bus_load_fence();
    peer_tail = ep->peer_ctl->tail;

    while (ep->cc_inflight_count > 0) {
        entry = &ep->cc_inflight[ep->cc_inflight_head];
        if (entry->fifo_head >= peer_tail) {
            break;
        }

        /* Keep reclaimed chunks on the cold end of the free list so they are
         * not immediately reused by the very next send. */
        uct_obmm_iface_requeue_cc_chunk(iface, entry->chunk_index);
        ep->cc_inflight_head = (ep->cc_inflight_head + 1) %
                               ep->cc_inflight_capacity;
        --ep->cc_inflight_count;
        ++count;
    }

    return count;
}


static ssize_t
uct_obmm_ep_am_bcopy_cc(uct_obmm_ep_t *ep, uct_obmm_iface_t *iface,
                        uint8_t id, uct_pack_callback_t pack_cb, void *arg)
{
    uct_obmm_fifo_element_t *elem;
    void                    *pack_dst;
    void                    *diag_tmp = NULL;
    void                    *chunk;
    uint16_t                 chunk_index;
    uint64_t                 head;
    size_t                   length;
    uint8_t                  owner_bit;
    uint8_t                  src_p0 = 0;
    uint8_t                  dst_p0 = 0;
    int                      diag_enabled;
    int                      handoff_enabled;
    ucs_status_t             status;
    static unsigned          diag_count;

    uct_obmm_ep_reclaim_chunks(ep);

    if (!uct_obmm_ep_has_tx_resource(ep) || (iface->cc_free_top == 0) ||
        (ep->cc_inflight_count == ep->cc_inflight_capacity)) {
        UCS_STATS_UPDATE_COUNTER(ep->super.stats, UCT_EP_STAT_NO_RES, 1);
        return UCS_ERR_NO_RESOURCE;
    }

    chunk_index = uct_obmm_iface_pop_cc_chunk(iface);
    chunk = UCS_PTR_BYTE_OFFSET(iface->cc_region->base,
                                (size_t)chunk_index * iface->cc_chunk_size);

    diag_enabled = uct_obmm_diag_bcopy_enabled();
    handoff_enabled = uct_obmm_diag_cc_handoff_enabled();
    pack_dst     = chunk;
    if (diag_enabled) {
        diag_tmp = ucs_malloc(iface->cc_chunk_size, "obmm_diag_pack");
        if (diag_tmp == NULL) {
            status = UCS_ERR_NO_MEMORY;
            goto err_push_chunk;
        }
        pack_dst = diag_tmp;
    }

    if (handoff_enabled) {
        status = uct_obmm_region_set_ownership(iface->cc_region, chunk,
                                               iface->cc_chunk_size,
                                               PROT_WRITE);
        if (status != UCS_OK) {
            goto err_free_tmp;
        }
    }

    length = pack_cb(pack_dst, arg);
    if (length > iface->cc_chunk_size) {
        ucs_error("obmm: pack_cb returned %zu > CC_CHUNK_SIZE=%zu",
                  length, iface->cc_chunk_size);
        status = UCS_ERR_INVALID_PARAM;
        goto err_free_tmp;
    }

    if (diag_enabled) {
        if (length > 0) {
            src_p0 = *(const uint8_t*)diag_tmp;
        }
        memcpy(chunk, diag_tmp, length);
        if (length > 0) {
            dst_p0 = *(const uint8_t*)chunk;
        }
    }

    if (handoff_enabled) {
        status = uct_obmm_region_set_ownership(iface->cc_region, chunk,
                                               iface->cc_chunk_size,
                                               PROT_NONE);
        if (status != UCS_OK) {
            goto err_free_tmp;
        }
    }

    status = uct_obmm_ep_reserve_slot(ep, &head);
    if (status != UCS_OK) {
        goto err_free_tmp;
    }

    elem = uct_obmm_slot_elem(ep->peer_elems, head, ep->fifo_mask,
                              ep->fifo_elem_size);
    elem->am_id      = id;
    elem->length     = 0;
    elem->generation = ep->expected_generation;
    elem->header     = uct_obmm_cc_hdr_pack((uint32_t)length, chunk_index,
                                            iface->cc_exporter_index);

    owner_bit = ((head / ep->fifo_size) & 1u) ? 0u :
                                                UCT_OBMM_FIFO_ELEM_FLAG_OWNER;
    ucs_memory_bus_store_fence();
    elem->flags = owner_bit | UCT_OBMM_FIFO_ELEM_FLAG_BCOPY |
                  UCT_OBMM_FIFO_ELEM_FLAG_CC_CHUNK;

    if (diag_enabled) {
        if (diag_count < 16) {
            fprintf(stderr, "obmmD XS h=%" PRIu64 " ch=%u e=%u s=%u d=%u\n",
                    head, chunk_index, iface->cc_exporter_index, src_p0, dst_p0);
            fflush(stderr);
            ++diag_count;
        }
        ucs_free(diag_tmp);
    }

    uct_obmm_ep_push_inflight(ep, head, chunk_index);

    UCT_TL_EP_STAT_OP(&ep->super, AM, BCOPY, length);
    uct_iface_trace_am(&iface->super.super, UCT_AM_TRACE_TYPE_SEND, id,
                       chunk, length, "TX: AM_BCOPY_CC");
    return (ssize_t)length;

err_free_tmp:
    ucs_free(diag_tmp);
err_push_chunk:
    uct_obmm_iface_push_cc_chunk(iface, chunk_index);
    return status;
}


ssize_t uct_obmm_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                             uct_pack_callback_t pack_cb, void *arg,
                             unsigned flags)
{
    uct_obmm_ep_t           *ep    = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    uct_obmm_iface_t        *iface = ucs_derived_of(tl_ep->iface,
                                                    uct_obmm_iface_t);
    uct_obmm_fifo_element_t *elem;
    void                    *desc;
    uint64_t                 head;
    size_t                   length;
    uint8_t                  owner_bit;
    ucs_status_t             status;

    /* flags (UCT_SEND_FLAG_PEER_CHECK etc.) are ignored: this transport
     * does not advertise EP_CHECK / keepalive in v1. */
    (void)flags;

    UCT_CHECK_AM_ID(id);

    if (iface->mode == UCT_OBMM_MEM_MODE_HYBRID) {
        return uct_obmm_ep_am_bcopy_cc(ep, iface, id, pack_cb, arg);
    }

    status = uct_obmm_ep_reserve_slot(ep, &head);
    if (status != UCS_OK) {
        return status;
    }

    elem = uct_obmm_slot_elem(ep->peer_elems, head, ep->fifo_mask,
                              ep->fifo_elem_size);
    desc = uct_obmm_slot_desc(ep->peer_descs, head, ep->fifo_mask,
                              ep->bcopy_seg_size);

    /* pack_cb writes pack_cb_ret bytes directly into the paired desc[N]
     * area. UCP guarantees pack_cb_ret <= cap.am.max_bcopy, which we set
     * to bcopy_seg_size. The asserts catch buggy direct UCT users in
     * debug builds; production safety relies on the iface cap contract. */
    length = pack_cb(desc, arg);
    ucs_assertv(length <= ep->bcopy_seg_size,
                "obmm: pack_cb returned %zu > bcopy_seg_size=%u",
                length, ep->bcopy_seg_size);
    ucs_assertv(length <= UINT16_MAX,
                "obmm: pack_cb returned %zu > UINT16_MAX", length);

    elem->am_id      = id;
    elem->length     = (uint16_t)length;
    elem->generation = ep->expected_generation;
    elem->header     = 0; /* unused for bcopy */

    owner_bit = ((head / ep->fifo_size) & 1u) ? 0u :
                                                UCT_OBMM_FIFO_ELEM_FLAG_OWNER;

    /* Release barrier: orders the desc[N] payload writes AND elem header
     * writes BEFORE the flags publish. Receiver pairs with bus_load_fence
     * after observing the flags byte. */
    ucs_memory_bus_store_fence();
    elem->flags = owner_bit | UCT_OBMM_FIFO_ELEM_FLAG_BCOPY;

    UCT_TL_EP_STAT_OP(&ep->super, AM, BCOPY, length);
    uct_iface_trace_am(&iface->super.super, UCT_AM_TRACE_TYPE_SEND, id,
                       desc, length, "TX: AM_BCOPY");
    return (ssize_t)length;
}


/* Returns true iff the peer's FIFO has at least one free slot, refreshing
 * cached_tail (with a bus_load_fence pair) before declaring "full". Mirrors
 * the resource check used by mm in pending_add. */
static UCS_F_ALWAYS_INLINE int
uct_obmm_ep_has_tx_resource(uct_obmm_ep_t *ep)
{
    uint64_t head = ep->peer_ctl->head;

    if ((head - ep->cached_tail) < ep->fifo_size) {
        return 1;
    }
    ucs_memory_bus_load_fence();
    ep->cached_tail = ep->peer_ctl->tail;
    return (head - ep->cached_tail) < ep->fifo_size;
}


ucs_status_t uct_obmm_ep_pending_add(uct_ep_h tl_ep, uct_pending_req_t *n,
                                     unsigned flags)
{
    uct_obmm_ep_t    *ep    = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    uct_obmm_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_obmm_iface_t);

    (void)flags;

    /* Resources may have appeared between the failed send and this call;
     * tell UCP to retry directly instead of queueing. mm uses the same
     * pattern (mm_ep.c:452-456). */
    if ((iface->mode == UCT_OBMM_MEM_MODE_HYBRID) &&
        (iface->cc_free_top == 0)) {
        uct_obmm_ep_reclaim_chunks(ep);
    }

    if (uct_obmm_ep_has_tx_resource(ep) &&
        ((iface->mode != UCT_OBMM_MEM_MODE_HYBRID) ||
         (iface->cc_free_top > 0))) {
        ucs_assert(ucs_arbiter_group_is_empty(&ep->arb_group));
        return UCS_ERR_BUSY;
    }

    UCS_STATIC_ASSERT(sizeof(uct_pending_req_priv_arb_t) <=
                      UCT_PENDING_REQ_PRIV_LEN);
    uct_pending_req_arb_group_push(&ep->arb_group, n);
    ucs_arbiter_group_schedule(&iface->arbiter, &ep->arb_group);
    UCT_TL_EP_STAT_PEND(&ep->super);

    return UCS_OK;
}


ucs_arbiter_cb_result_t
uct_obmm_ep_process_pending(ucs_arbiter_t *arbiter, ucs_arbiter_group_t *group,
                            ucs_arbiter_elem_t *elem, void *arg)
{
    uct_obmm_ep_t     *ep    = ucs_container_of(group, uct_obmm_ep_t, arb_group);
    unsigned          *count = (unsigned*)arg;
    uct_pending_req_t *req;
    ucs_status_t       status;

    /* Refresh cached tail so the request callback's am_short/am_bcopy sees
     * the freshest peer state and is not falsely starved. */
    if (!uct_obmm_ep_has_tx_resource(ep)) {
        return UCS_ARBITER_CB_RESULT_RESCHED_GROUP;
    }

    req    = ucs_container_of(elem, uct_pending_req_t, priv);
    status = req->func(req);

    if (status == UCS_OK) {
        ++(*count);
        return UCS_ARBITER_CB_RESULT_REMOVE_ELEM;
    } else if (status == UCS_INPROGRESS) {
        ++(*count);
        return UCS_ARBITER_CB_RESULT_NEXT_GROUP;
    }

    /* NO_RESOURCE (or any other transient): keep the request and try
     * again the next time iface_progress runs. */
    return UCS_ARBITER_CB_RESULT_RESCHED_GROUP;
}


typedef struct {
    uct_pending_purge_callback_t cb;
    void                        *arg;
} uct_obmm_purge_args_t;


static ucs_arbiter_cb_result_t
uct_obmm_ep_arbiter_purge_cb(ucs_arbiter_t *arbiter, ucs_arbiter_group_t *group,
                             ucs_arbiter_elem_t *elem, void *arg)
{
    uct_obmm_ep_t         *ep   = ucs_container_of(group, uct_obmm_ep_t,
                                                   arb_group);
    uct_obmm_purge_args_t *args = arg;
    uct_pending_req_t     *req;

    req = ucs_container_of(elem, uct_pending_req_t, priv);
    if (args->cb != NULL) {
        args->cb(req, args->arg);
    } else {
        ucs_warn("obmm: ep=%p canceling pending request %p", ep, req);
    }
    return UCS_ARBITER_CB_RESULT_REMOVE_ELEM;
}


void uct_obmm_ep_pending_purge(uct_ep_h tl_ep,
                               uct_pending_purge_callback_t cb, void *arg)
{
    uct_obmm_ep_t         *ep    = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    uct_obmm_iface_t      *iface = ucs_derived_of(tl_ep->iface,
                                                  uct_obmm_iface_t);
    uct_obmm_purge_args_t  args  = {cb, arg};

    ucs_arbiter_group_purge(&iface->arbiter, &ep->arb_group,
                            uct_obmm_ep_arbiter_purge_cb, &args);
}
