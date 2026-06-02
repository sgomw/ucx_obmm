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
#include "obmm_atomic.h"

#include <uct/base/uct_log.h>
#include <uct/base/uct_iface.h>
#include <ucs/arch/atomic.h>
#include <ucs/arch/cpu.h>
#include <ucs/debug/log.h>
#include <ucs/sys/math.h>

#include <libobmm.h>

#include <string.h>

static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_reserve_slot(uct_obmm_ep_t *ep, uint64_t *head_p);

static UCS_CLASS_INIT_FUNC(uct_obmm_ep_t, const uct_ep_params_t *params)
{
    uct_obmm_iface_t             *iface = ucs_derived_of(params->iface,
                                                         uct_obmm_iface_t);
    uct_obmm_md_t                *md    = ucs_derived_of(iface->super.md,
                                                         uct_obmm_md_t);
    const uct_obmm_device_addr_t *daddr;
    const uct_obmm_iface_addr_t  *iaddr;
    uct_obmm_region_t            *nc_region;
    uct_obmm_region_t            *cc_region = NULL;
    uct_obmm_pool_t               peer_pool;
    void                         *peer_slot;
    ucs_status_t                  status;

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);

    ucs_arbiter_group_init(&self->arb_group);

    daddr = (const uct_obmm_device_addr_t*)params->dev_addr;
    iaddr = (const uct_obmm_iface_addr_t*)params->iface_addr;

    /* Reject incompatible NC geometry. */
    if ((iaddr->slot_count != UCT_OBMM_POOL_SLOT_COUNT) ||
        (iaddr->short_lane_count != UCT_OBMM_SHORT_LANE_COUNT) ||
        (iaddr->fifo_size != iface->fifo_size) ||
        (iaddr->fifo_elem_size != iface->fifo_elem_size) ||
        (iaddr->bcopy_seg_size != iface->bcopy_seg_size)) {
        ucs_error("obmm: peer NC geometry (slots=%u lanes=%u fifo=%u elem=%u "
                  "seg=%u) differs from local (slots=%u lanes=%u fifo=%u "
                  "elem=%u seg=%u); ep_create rejected",
                  iaddr->slot_count, iaddr->short_lane_count,
                  iaddr->fifo_size, iaddr->fifo_elem_size,
                  iaddr->bcopy_seg_size,
                  UCT_OBMM_POOL_SLOT_COUNT, UCT_OBMM_SHORT_LANE_COUNT,
                  iface->fifo_size, iface->fifo_elem_size,
                  iface->bcopy_seg_size);
        return UCS_ERR_UNREACHABLE;
    }

    /* Reject incompatible CC geometry. */
    if (((iaddr->cc_enabled != 0) != (iface->cc_enabled != 0)) ||
        (iaddr->cc_enabled && (iaddr->cc_buf_size != iface->cc_buf_size))) {
        ucs_error("obmm: peer CC geometry (cc_en=%u cc_buf=%u) "
                  "differs from local (cc_en=%d cc_buf=%u); ep_create rejected",
                  iaddr->cc_enabled, iaddr->cc_buf_size,
                  iface->cc_enabled, iface->cc_buf_size);
        return UCS_ERR_UNREACHABLE;
    }

    /* Find the local NC region (export-for-self, import-for-remote). */
    {
        uct_obmm_region_t *exp_r;
        uct_obmm_eid_t     eid;

        eid.hi = daddr->exporter_deid_hi;
        eid.lo = daddr->exporter_deid_lo;

        exp_r     = uct_obmm_md_export_region(md);
        nc_region = NULL;
        if ((exp_r != NULL) &&
            (exp_r->info.exporter_dcna == daddr->exporter_dcna) &&
            (exp_r->info.exporter_deid.hi == eid.hi) &&
            (exp_r->info.exporter_deid.lo == eid.lo)) {
            nc_region = exp_r;
        } else {
            nc_region = uct_obmm_md_find_import_region(md,
                                                       daddr->exporter_dcna,
                                                       &eid);
        }
    }
    if (nc_region == NULL) {
        ucs_error("obmm: ep_create cannot find NC region for peer "
                  "dcna=0x%lx deid=0x%lx:0x%lx",
                  (unsigned long)daddr->exporter_dcna,
                  (unsigned long)daddr->exporter_deid_hi,
                  (unsigned long)daddr->exporter_deid_lo);
        return UCS_ERR_UNREACHABLE;
    }

    /* Find the local CC import region for this peer. */
    if (iface->cc_enabled && iaddr->cc_enabled) {
        uct_obmm_eid_t eid;

        eid.hi = daddr->exporter_deid_hi;
        eid.lo = daddr->exporter_deid_lo;

        /* Try self-loopback first: if the peer is our own CC export.  */
        {
            uct_obmm_region_t *cc_exp_r = uct_obmm_md_cc_export_region(md);

            if ((cc_exp_r != NULL) &&
                (cc_exp_r->info.exporter_dcna == daddr->exporter_dcna) &&
                (cc_exp_r->info.exporter_deid.hi == eid.hi) &&
                (cc_exp_r->info.exporter_deid.lo == eid.lo)) {
                cc_region = cc_exp_r;
            }
        }
        if (cc_region == NULL) {
            cc_region = uct_obmm_md_find_cc_import_region(md,
                                                          daddr->exporter_dcna,
                                                          &eid);
        }
        if (cc_region == NULL) {
            ucs_error("obmm: ep_create cannot find CC region for peer "
                      "dcna=0x%lx deid=0x%lx:0x%lx",
                      (unsigned long)daddr->exporter_dcna,
                      (unsigned long)daddr->exporter_deid_hi,
                      (unsigned long)daddr->exporter_deid_lo);
            return UCS_ERR_UNREACHABLE;
        }
    }

    status = uct_obmm_pool_open(nc_region->base, nc_region->length, &peer_pool);
    if (status != UCS_OK) {
        ucs_error("obmm: failed to open peer NC pool: %s",
                  ucs_status_string(status));
        return status;
    }

    if (peer_pool.slot_count != iaddr->slot_count) {
        ucs_error("obmm: peer pool slot_count %u inconsistent with "
                  "iface_addr slot_count %u",
                  peer_pool.slot_count, iaddr->slot_count);
        return UCS_ERR_INVALID_PARAM;
    }

    if (iaddr->slot_index >= peer_pool.slot_count) {
        ucs_error("obmm: peer slot_index %u out of range (slot_count=%u)",
                  iaddr->slot_index, peer_pool.slot_count);
        return UCS_ERR_INVALID_PARAM;
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
    self->cached_tail         = self->peer_ctl->tail;
    self->expected_generation = iaddr->generation;
    self->fifo_size           = iaddr->fifo_size;
    self->fifo_mask           = iaddr->fifo_size - 1u;
    self->fifo_elem_size      = iaddr->fifo_elem_size;
    self->bcopy_seg_size      = iaddr->bcopy_seg_size;
    self->peer_dcna           = daddr->exporter_dcna;
    self->peer_deid_hi        = daddr->exporter_deid_hi;
    self->peer_deid_lo        = daddr->exporter_deid_lo;
    self->peer_slot_index     = iaddr->slot_index;
    self->peer_pid            = iaddr->pid;

    /* CC state */
    self->cc_enabled          = iface->cc_enabled && (iaddr->cc_enabled != 0);
    self->cc_buf_size         = iaddr->cc_buf_size;
    self->cc_num_bufs         = iface->cc_num_bufs;
    self->cc_peer_region      = cc_region;
    self->cc_peer_fd          = (cc_region != NULL) ? cc_region->fd : -1;

    if (self->cc_enabled && (cc_region != NULL)) {
        /* Cache the peer CC import on the iface so the receive progress
         * loop can locate CC payload data without per-ep dispatch.
         * In a single-peer topology (2 nodes), all CC data comes from
         * the same peer, so a single cached pointer is sufficient. */
        iface->cc_peer_region = cc_region;

        ucs_debug("obmm: ep CC enabled: peer CC import at %p (fd=%d) "
                  "buf_size=%u num_bufs=%u",
                  cc_region->base, self->cc_peer_fd,
                  self->cc_buf_size, self->cc_num_bufs);
    }

    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_obmm_ep_t)
{
    uct_obmm_ep_pending_purge(&self->super.super, NULL, NULL);
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
           (iaddr->slot_count == UCT_OBMM_POOL_SLOT_COUNT) &&
           (iaddr->short_lane_count == UCT_OBMM_SHORT_LANE_COUNT) &&
           (iaddr->fifo_size == ep->fifo_size) &&
           (iaddr->fifo_elem_size == ep->fifo_elem_size) &&
           (iaddr->bcopy_seg_size == ep->bcopy_seg_size) &&
           (iaddr->slot_index == ep->peer_slot_index) &&
           (iaddr->generation == ep->expected_generation) &&
           (iaddr->cc_enabled == ep->cc_enabled) &&
           (iaddr->cc_buf_size == ep->cc_buf_size);
}


ucs_status_t uct_obmm_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t header,
                                  const void *payload, unsigned length)
{
    uct_obmm_ep_t           *ep    = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    uct_obmm_iface_t        *iface = ucs_derived_of(tl_ep->iface,
                                                    uct_obmm_iface_t);
    uct_obmm_fifo_element_t *elem;
    void                    *short_data;
    size_t                   payload_total = sizeof(header) + length;
    uint64_t                 head;
    uint8_t                  owner_bit;
    ucs_status_t             status;

    UCT_CHECK_AM_ID(id);
    UCT_CHECK_LENGTH(payload_total, 0,
                     uct_obmm_fifo_max_short(ep->fifo_elem_size),
                     "am_short");

    status = uct_obmm_ep_reserve_slot(ep, &head);
    if (status != UCS_OK) {
        return status;
    }

    elem             = uct_obmm_slot_elem(ep->peer_elems, head, ep->fifo_mask,
                                          ep->fifo_elem_size);
    elem->am_id      = id;
    elem->length     = (uint16_t)payload_total;
    elem->generation = ep->expected_generation;
    elem->header     = header;
    if (length > 0) {
        memcpy(elem + 1, payload, length);
    }
    short_data = (char*)elem + ucs_offsetof(uct_obmm_fifo_element_t, header);

    owner_bit = (head & ep->fifo_size) ? 0u :
                                        UCT_OBMM_FIFO_ELEM_FLAG_OWNER;
    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, id,
                       short_data, payload_total, "TX: AM_SHORT_FIFO");
    ucs_memory_bus_store_fence();
    elem->flags = owner_bit;

    UCT_TL_EP_STAT_OP(&ep->super, AM, SHORT, payload_total);
    return UCS_OK;
}


/* Reserve one slot in the peer's FIFO, returning the head index that was
 * claimed. Use CAS (not FAA): if an FAA claim succeeds and the FIFO then
 * turns out to be full, the head bump cannot be rolled back and would
 * leave a permanent hole. On aarch64 NC mappings this must be an explicit
 * LSE CAS, not a compiler-default LL/SC atomic. */
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

        if (uct_obmm_atomic_bool_cswap64(&ep->peer_ctl->head, head,
                                         head + 1)) {
            *head_p = head;
            return UCS_OK;
        }
    }
}


/* CC-accelerated am_bcopy: reserve an NC FIFO slot, write the payload
 * into the sender's CC export region, and publish the NC FIFO element
 * with FLAG_CC + the CC buffer offset. */
static UCS_F_ALWAYS_INLINE ssize_t
uct_obmm_ep_am_bcopy_cc(uct_obmm_ep_t *ep, uint8_t id,
                        uct_pack_callback_t pack_cb, void *arg,
                        uint64_t head)
{
    uct_obmm_iface_t        *iface  = ucs_derived_of(ep->super.super.iface,
                                                     uct_obmm_iface_t);
    uct_obmm_fifo_element_t *elem;
    ucs_status_t             status;
    uint32_t                 cc_idx;
    uint64_t                 cc_offset;
    void                    *cc_ptr;
    uint8_t                  owner_bit;
    size_t                   length;

    cc_idx    = (uint32_t)(head & ep->fifo_mask) % iface->cc_num_bufs;
    cc_offset = (uint64_t)cc_idx * iface->cc_buf_size;
    cc_ptr    = (char*)iface->cc_region->base + cc_offset;

    /* Acquire write ownership on the sender side. */
    status = obmm_set_ownership(iface->cc_region->fd, cc_ptr,
                                (char*)cc_ptr + iface->cc_buf_size,
                                PROT_READ | PROT_WRITE);
    if (status != 0) {
        ucs_error("obmm: CC sender set_ownership(WRITE) at offset 0x%"
                  PRIx64 " failed: %m", cc_offset);
        return UCS_ERR_NO_RESOURCE;
    }

    /* Pack directly into the CC buffer. */
    length = pack_cb(cc_ptr, arg);
    ucs_assertv(length <= iface->cc_buf_size,
                "obmm: CC pack_cb returned %zu > cc_buf_size=%u",
                length, iface->cc_buf_size);
    ucs_assertv(length <= UINT16_MAX,
                "obmm: CC pack_cb returned %zu > UINT16_MAX", length);

    /* Release write ownership. */
    status = obmm_set_ownership(iface->cc_region->fd, cc_ptr,
                                (char*)cc_ptr + iface->cc_buf_size,
                                PROT_NONE);
    if (status != 0) {
        ucs_error("obmm: CC sender set_ownership(NONE) at offset 0x%"
                  PRIx64 " failed: %m", cc_offset);
        return UCS_ERR_NO_RESOURCE;
    }

    /* Publish the NC FIFO element with FLAG_CC. */
    elem             = uct_obmm_slot_elem(ep->peer_elems, head, ep->fifo_mask,
                                          ep->fifo_elem_size);
    elem->am_id      = id;
    elem->length     = (uint16_t)length;
    elem->generation = ep->expected_generation;
    elem->header     = cc_offset;

    owner_bit = (head & ep->fifo_size) ? 0u :
                                        UCT_OBMM_FIFO_ELEM_FLAG_OWNER;

    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, id,
                       cc_ptr, length, "TX: AM_BCOPY_CC");

    ucs_memory_bus_store_fence();
    elem->flags = owner_bit | UCT_OBMM_FIFO_ELEM_FLAG_CC;

    return (ssize_t)length;
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

    (void)flags;

    UCT_CHECK_AM_ID(id);

    status = uct_obmm_ep_reserve_slot(ep, &head);
    if (status != UCS_OK) {
        return status;
    }

    /* Route to CC path when enabled and we expect a large payload.
     * The real length is determined by pack_cb, but we don't know it yet.
     * We use the threshold as a hint: if the caller (UCP) would have
     * chosen bcopy for a message >= threshold, we assume the payload
     * is large and route to CC. */
    if (ep->cc_enabled && iface->cc_region != NULL) {
        return uct_obmm_ep_am_bcopy_cc(ep, id, pack_cb, arg, head);
    }

    /* NC bcopy path (original). */
    elem = uct_obmm_slot_elem(ep->peer_elems, head, ep->fifo_mask,
                              ep->fifo_elem_size);
    desc = uct_obmm_slot_desc(ep->peer_descs, head, ep->fifo_mask,
                              ep->bcopy_seg_size);

    length = pack_cb(desc, arg);
    ucs_assertv(length <= ep->bcopy_seg_size,
                "obmm: pack_cb returned %zu > bcopy_seg_size=%u",
                length, ep->bcopy_seg_size);
    ucs_assertv(length <= UINT16_MAX,
                "obmm: pack_cb returned %zu > UINT16_MAX", length);

    elem->am_id      = id;
    elem->length     = (uint16_t)length;
    elem->generation = ep->expected_generation;
    elem->header     = 0;

    owner_bit = (head & ep->fifo_size) ? 0u :
                                        UCT_OBMM_FIFO_ELEM_FLAG_OWNER;

    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, id,
                       desc, length, "TX: AM_BCOPY");

    ucs_memory_bus_store_fence();
    elem->flags = owner_bit | UCT_OBMM_FIFO_ELEM_FLAG_BCOPY;

    UCT_TL_EP_STAT_OP(&ep->super, AM, BCOPY, length);
    return (ssize_t)length;
}


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

    if (uct_obmm_ep_has_tx_resource(ep) &&
        ucs_arbiter_group_is_empty(&ep->arb_group)) {
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
    uct_obmm_iface_t  *iface = ucs_derived_of(ep->super.super.iface,
                                              uct_obmm_iface_t);
    unsigned          *count = (unsigned*)arg;
    uct_pending_req_t *req;
    ucs_status_t       status;

    if (*count >= iface->pending_quota) {
        return UCS_ARBITER_CB_RESULT_STOP;
    }

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