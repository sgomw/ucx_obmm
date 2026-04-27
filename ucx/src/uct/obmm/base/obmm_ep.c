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
#include <ucs/arch/atomic.h>
#include <ucs/arch/cpu.h>
#include <ucs/debug/log.h>
#include <ucs/sys/math.h>

#include <string.h>


static UCS_CLASS_INIT_FUNC(uct_obmm_ep_t, const uct_ep_params_t *params)
{
    uct_obmm_iface_t             *iface = ucs_derived_of(params->iface,
                                                         uct_obmm_iface_t);
    uct_obmm_md_t                *md    = ucs_derived_of(iface->super.super.md,
                                                         uct_obmm_md_t);
    const uct_obmm_device_addr_t *daddr;
    const uct_obmm_iface_addr_t  *iaddr;
    uct_obmm_region_t            *region;
    uct_obmm_pool_t               peer_pool;
    void                         *peer_slot;
    ucs_status_t                  status;

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super.super);

    daddr = (const uct_obmm_device_addr_t*)params->dev_addr;
    iaddr = (const uct_obmm_iface_addr_t*)params->iface_addr;

    /* Reject incompatible geometry. UCX wireup should already have filtered
     * this out via is_reachable_v2, but double-check. */
    if ((iaddr->fifo_size != iface->fifo_size) ||
        (iaddr->fifo_elem_size != iface->fifo_elem_size)) {
        ucs_error("obmm: peer FIFO geometry (size=%u elem=%u) differs from "
                  "local (size=%u elem=%u); ep_create rejected",
                  iaddr->fifo_size, iaddr->fifo_elem_size,
                  iface->fifo_size, iface->fifo_elem_size);
        return UCS_ERR_UNREACHABLE;
    }

    /* Find the local region (export-for-self, import-for-remote) that maps
     * the peer's exporter coordinates. */
    {
        uct_obmm_region_t *exp_r;
        uct_obmm_eid_t     eid;

        eid.hi = daddr->exporter_deid_hi;
        eid.lo = daddr->exporter_deid_lo;

        exp_r  = uct_obmm_md_export_region(md);
        region = NULL;
        if ((exp_r != NULL) &&
            (exp_r->info.exporter_dcna == daddr->exporter_dcna) &&
            (exp_r->info.exporter_deid.hi == eid.hi) &&
            (exp_r->info.exporter_deid.lo == eid.lo)) {
            region = exp_r;
        } else {
            region = uct_obmm_md_find_import_region(md, daddr->exporter_dcna,
                                                    &eid);
        }
    }
    if (region == NULL) {
        ucs_error("obmm: ep_create cannot find region for peer "
                  "dcna=0x%lx deid=0x%lx:0x%lx",
                  (unsigned long)daddr->exporter_dcna,
                  (unsigned long)daddr->exporter_deid_hi,
                  (unsigned long)daddr->exporter_deid_lo);
        return UCS_ERR_UNREACHABLE;
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

    if (peer_pool.slot_size !=
        uct_obmm_slot_stride(iaddr->fifo_size, iaddr->fifo_elem_size)) {
        ucs_error("obmm: peer pool slot_size %u inconsistent with iface_addr "
                  "geometry (size=%u elem=%u)",
                  peer_pool.slot_size, iaddr->fifo_size, iaddr->fifo_elem_size);
        return UCS_ERR_INVALID_PARAM;
    }

    peer_slot = uct_obmm_pool_slot_ptr(&peer_pool, iaddr->slot_index);

    self->peer_ctl            = uct_obmm_slot_ctl(peer_slot);
    self->peer_elems          = uct_obmm_slot_elems(peer_slot);
    self->cached_tail         = self->peer_ctl->tail;
    self->expected_generation = iaddr->generation;
    self->fifo_size           = iaddr->fifo_size;
    self->fifo_mask           = iaddr->fifo_size - 1u;
    self->fifo_elem_size      = iaddr->fifo_elem_size;
    self->peer_dcna           = daddr->exporter_dcna;
    self->peer_deid_hi        = daddr->exporter_deid_hi;
    self->peer_deid_lo        = daddr->exporter_deid_lo;
    self->peer_slot_index     = iaddr->slot_index;
    self->peer_pid            = iaddr->pid;
    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_obmm_ep_t)
{
    /* Nothing to release: peer pool memory is owned by the MD. */
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

    return (daddr->exporter_dcna == ep->peer_dcna) &&
           (daddr->exporter_deid_hi == ep->peer_deid_hi) &&
           (daddr->exporter_deid_lo == ep->peer_deid_lo) &&
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

    UCT_CHECK_AM_ID(id);
    UCT_CHECK_LENGTH(payload_total, 0,
                     ep->fifo_elem_size - sizeof(uct_obmm_fifo_element_t),
                     "am_short");

    /* Reserve a slot in the peer's FIFO via load + CAS. FAA cannot be used:
     * if FAA succeeds but the FIFO turns out to be full, the bumped head
     * value can never be rolled back across hosts, leaving a permanent gap
     * the receiver will block on (its progress loop walks slots in order
     * and stops at any slot whose owner bit hasn't flipped). CAS lets us
     * decide capacity *before* committing the head bump.
     *
     * Multiple senders (across processes / hosts) compete on the same head
     * cell, so we retry on CAS miss. */
    for (;;) {
        head = ep->peer_ctl->head;

        if ((head - ep->cached_tail) >= ep->fifo_size) {
            ucs_memory_bus_load_fence();
            ep->cached_tail = ep->peer_ctl->tail;
            if ((head - ep->cached_tail) >= ep->fifo_size) {
                UCS_STATS_UPDATE_COUNTER(ep->super.stats, UCT_EP_STAT_NO_RES,
                                         1);
                return UCS_ERR_NO_RESOURCE;
            }
        }

        if (ucs_atomic_bool_cswap64(&ep->peer_ctl->head, head, head + 1)) {
            break;
        }
        /* Lost the race; another sender claimed this slot. Retry. */
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
 * claimed. See am_short above for why load+CAS (not FAA). */
static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_reserve_slot(uct_obmm_ep_t *ep, uint64_t *head_p)
{
    uint64_t head;

    for (;;) {
        head = ep->peer_ctl->head;

        if ((head - ep->cached_tail) >= ep->fifo_size) {
            ucs_memory_bus_load_fence();
            ep->cached_tail = ep->peer_ctl->tail;
            if ((head - ep->cached_tail) >= ep->fifo_size) {
                UCS_STATS_UPDATE_COUNTER(ep->super.stats, UCT_EP_STAT_NO_RES,
                                         1);
                return UCS_ERR_NO_RESOURCE;
            }
        }

        if (ucs_atomic_bool_cswap64(&ep->peer_ctl->head, head, head + 1)) {
            *head_p = head;
            return UCS_OK;
        }
    }
}


ssize_t uct_obmm_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                             uct_pack_callback_t pack_cb, void *arg,
                             unsigned flags)
{
    uct_obmm_ep_t           *ep    = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    uct_obmm_iface_t        *iface = ucs_derived_of(tl_ep->iface,
                                                    uct_obmm_iface_t);
    uct_obmm_fifo_element_t *elem;
    uint64_t                 head;
    size_t                   length;
    uint8_t                  owner_bit;
    ucs_status_t             status;

    /* flags (UCT_SEND_FLAG_PEER_CHECK etc.) are ignored: this transport
     * does not advertise EP_CHECK / keepalive in v1. */
    (void)flags;

    UCT_CHECK_AM_ID(id);

    status = uct_obmm_ep_reserve_slot(ep, &head);
    if (status != UCS_OK) {
        return status;
    }

    elem = uct_obmm_slot_elem(ep->peer_elems, head, ep->fifo_mask,
                              ep->fifo_elem_size);

    /* pack_cb writes pack_cb_ret bytes directly into the slot's payload
     * area. UCP guarantees pack_cb_ret <= cap.am.max_bcopy, which we set
     * to (fifo_elem_size - sizeof(elem_hdr)). */
    length = pack_cb((void*)(elem + 1), arg);

    elem->am_id      = id;
    elem->length     = (uint16_t)length;
    elem->generation = ep->expected_generation;
    elem->header     = 0; /* unused for bcopy */

    owner_bit = ((head / ep->fifo_size) & 1u) ? 0u :
                                                UCT_OBMM_FIFO_ELEM_FLAG_OWNER;

    ucs_memory_bus_store_fence();
    elem->flags = owner_bit | UCT_OBMM_FIFO_ELEM_FLAG_BCOPY;

    UCT_TL_EP_STAT_OP(&ep->super, AM, BCOPY, length);
    uct_iface_trace_am(&iface->super.super, UCT_AM_TRACE_TYPE_SEND, id,
                       (void*)(elem + 1), length, "TX: AM_BCOPY");
    return (ssize_t)length;
}


ucs_status_t uct_obmm_ep_pending_add(uct_ep_h tl_ep, uct_pending_req_t *n,
                                     unsigned flags)
{
    uct_obmm_ep_t *ep = ucs_derived_of(tl_ep, uct_obmm_ep_t);

    /* v1 does not implement a real pending queue. Tell UCP that resources
     * may be available again so it retries the send via its own progress
     * loop. The receiver-side iface_progress drains the FIFO, so the
     * NO_RESOURCE condition is transient and bounded. */
    (void)n;
    (void)flags;

    /* Refresh cached tail so any subsequent capacity test sees the latest
     * value before the caller retries. */
    ucs_memory_bus_load_fence();
    ep->cached_tail = ep->peer_ctl->tail;

    return UCS_ERR_BUSY;
}
