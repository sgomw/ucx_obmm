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
#include <ucs/debug/log.h>

#include <string.h>
#include <unistd.h>


static UCS_F_ALWAYS_INLINE uint8_t
uct_obmm_ep_mailbox_bank(uct_obmm_iface_t *iface,
                         const uct_obmm_device_addr_t *daddr)
{
    return ((iface->region->info.exporter_dcna == daddr->exporter_dcna) &&
            (iface->region->info.exporter_deid.hi == daddr->exporter_deid_hi) &&
            (iface->region->info.exporter_deid.lo == daddr->exporter_deid_lo)) ?
           UCT_OBMM_MAILBOX_BANK_LOCAL : UCT_OBMM_MAILBOX_BANK_REMOTE;
}


static void
uct_obmm_ep_register_rx_lane(uct_obmm_iface_t *iface, uct_obmm_ep_t *ep,
                             void *peer_slot)
{
    uct_obmm_rx_lane_t *lane = &iface->rx_lanes[ep->mailbox_bank]
                                              [ep->peer_slot_index];
    void               *lane_base;

    if (!lane->active ||
        (lane->sender_generation != ep->expected_generation) ||
        (lane->peer_dcna != ep->peer_dcna) ||
        (lane->peer_deid_hi != ep->peer_deid_hi) ||
        (lane->peer_deid_lo != ep->peer_deid_lo)) {
        lane_base = uct_obmm_slot_lane(peer_slot, ep->mailbox_bank,
                                       iface->slot_index, iface->fifo_size,
                                       iface->fifo_elem_size,
                                       iface->bcopy_seg_size);
        lane->ctl               = uct_obmm_lane_ctl(lane_base);
        lane->elems             = uct_obmm_lane_elems(lane_base);
        lane->descs             = uct_obmm_lane_descs(lane_base,
                                                      iface->fifo_size,
                                                      iface->fifo_elem_size);
        lane->peer_dcna         = ep->peer_dcna;
        lane->peer_deid_hi      = ep->peer_deid_hi;
        lane->peer_deid_lo      = ep->peer_deid_lo;
        lane->peer_pid          = ep->peer_pid;
        lane->sender_generation = ep->expected_generation;
        lane->rx_index          = (lane->ctl->tail_generation ==
                                   ep->expected_generation) ?
                                  lane->ctl->tail : 0;
        lane->refs              = 0;
        lane->active            = 1;
    }

    lane->refs++;
}


static void
uct_obmm_ep_unregister_rx_lane(uct_obmm_iface_t *iface, uct_obmm_ep_t *ep)
{
    uct_obmm_rx_lane_t *lane = &iface->rx_lanes[ep->mailbox_bank]
                                              [ep->peer_slot_index];

    if (!lane->active ||
        (lane->sender_generation != ep->expected_generation) ||
        (lane->peer_dcna != ep->peer_dcna) ||
        (lane->peer_deid_hi != ep->peer_deid_hi) ||
        (lane->peer_deid_lo != ep->peer_deid_lo)) {
        return;
    }

    if (lane->refs > 0) {
        lane->refs--;
    }
    if (lane->refs == 0) {
        memset(lane, 0, sizeof(*lane));
    }
}


static UCS_F_ALWAYS_INLINE void
uct_obmm_ep_update_cached_tail(uct_obmm_ep_t *ep, uint32_t local_generation)
{
    ucs_memory_bus_load_fence();
    if (ep->tx_ctl->tail_generation == local_generation) {
        ep->cached_tail = ep->tx_ctl->tail;
    }
}


static UCS_F_ALWAYS_INLINE int
uct_obmm_ep_has_tx_resource(uct_obmm_ep_t *ep)
{
    uct_obmm_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                             uct_obmm_iface_t);

    if ((uint32_t)(ep->tx_index - ep->cached_tail) < ep->fifo_size) {
        return 1;
    }

    uct_obmm_ep_update_cached_tail(ep, iface->generation);
    return (uint32_t)(ep->tx_index - ep->cached_tail) < ep->fifo_size;
}


static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_reserve_slot(uct_obmm_ep_t *ep, uint32_t *index_p)
{
    uct_obmm_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                             uct_obmm_iface_t);

    if (!uct_obmm_ep_has_tx_resource(ep)) {
        UCS_STATS_UPDATE_COUNTER(ep->super.stats, UCT_EP_STAT_NO_RES, 1);
        return UCS_ERR_NO_RESOURCE;
    }

    *index_p = ep->tx_index;
    (void)iface;
    return UCS_OK;
}


static UCS_CLASS_INIT_FUNC(uct_obmm_ep_t, const uct_ep_params_t *params)
{
    uct_obmm_iface_t             *iface = ucs_derived_of(params->iface,
                                                         uct_obmm_iface_t);
    uct_obmm_md_t                *md    = ucs_derived_of(iface->super.md,
                                                         uct_obmm_md_t);
    const uct_obmm_device_addr_t *daddr;
    const uct_obmm_iface_addr_t  *iaddr;
    uct_obmm_region_t            *region;
    uct_obmm_pool_t               peer_pool;
    void                         *peer_slot;
    void                         *tx_lane;
    ucs_status_t                  status;
    uct_obmm_eid_t                eid;

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);

    ucs_arbiter_group_init(&self->arb_group);

    daddr = (const uct_obmm_device_addr_t*)params->dev_addr;
    iaddr = (const uct_obmm_iface_addr_t*)params->iface_addr;

    if ((iaddr->fifo_size != iface->fifo_size) ||
        (iaddr->fifo_elem_size != iface->fifo_elem_size) ||
        (iaddr->bcopy_seg_size != iface->bcopy_seg_size)) {
        ucs_error("obmm: peer geometry (fifo=%u elem=%u seg=%u) differs from "
                  "local (fifo=%u elem=%u seg=%u); ep_create rejected",
                  iaddr->fifo_size, iaddr->fifo_elem_size,
                  iaddr->bcopy_seg_size,
                  iface->fifo_size, iface->fifo_elem_size,
                  iface->bcopy_seg_size);
        return UCS_ERR_UNREACHABLE;
    }

    eid.hi = daddr->exporter_deid_hi;
    eid.lo = daddr->exporter_deid_lo;

    region = NULL;
    if ((iface->region->info.exporter_dcna == daddr->exporter_dcna) &&
        (iface->region->info.exporter_deid.hi == eid.hi) &&
        (iface->region->info.exporter_deid.lo == eid.lo)) {
        region = iface->region;
    } else {
        region = uct_obmm_md_find_import_region(md, daddr->exporter_dcna, &eid);
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
        uct_obmm_slot_stride(iaddr->fifo_size, iaddr->fifo_elem_size,
                             iaddr->bcopy_seg_size)) {
        ucs_error("obmm: peer pool slot_size %u inconsistent with iface_addr "
                  "geometry (fifo=%u elem=%u seg=%u)",
                  peer_pool.slot_size, iaddr->fifo_size,
                  iaddr->fifo_elem_size, iaddr->bcopy_seg_size);
        return UCS_ERR_INVALID_PARAM;
    }

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
    self->mailbox_bank        = uct_obmm_ep_mailbox_bank(iface, daddr);

    tx_lane        = uct_obmm_slot_lane(iface->slot, self->mailbox_bank,
                                        self->peer_slot_index, iface->fifo_size,
                                        iface->fifo_elem_size,
                                        iface->bcopy_seg_size);
    self->tx_ctl   = uct_obmm_lane_ctl(tx_lane);
    self->tx_elems = uct_obmm_lane_elems(tx_lane);
    self->tx_descs = uct_obmm_lane_descs(tx_lane, iface->fifo_size,
                                         iface->fifo_elem_size);
    self->tx_index = self->tx_ctl->head;
    ucs_memory_bus_load_fence();
    self->cached_tail = (self->tx_ctl->tail_generation == iface->generation) ?
                        self->tx_ctl->tail : self->tx_index;

    peer_slot = uct_obmm_pool_slot_ptr(&peer_pool, iaddr->slot_index);
    uct_obmm_ep_register_rx_lane(iface, self, peer_slot);
    return UCS_OK;
}


static UCS_CLASS_CLEANUP_FUNC(uct_obmm_ep_t)
{
    uct_obmm_iface_t *iface = ucs_derived_of(self->super.super.iface,
                                             uct_obmm_iface_t);

    uct_obmm_ep_pending_purge(&self->super.super, NULL, NULL);
    uct_obmm_ep_unregister_rx_lane(iface, self);
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
    uint32_t                 index;

    UCT_CHECK_AM_ID(id);
    UCT_CHECK_LENGTH(payload_total, 0,
                     ep->fifo_elem_size - sizeof(uct_obmm_fifo_element_t),
                     "am_short");

    if (uct_obmm_ep_reserve_slot(ep, &index) != UCS_OK) {
        return UCS_ERR_NO_RESOURCE;
    }

    elem = uct_obmm_lane_elem(ep->tx_elems, index, ep->fifo_mask,
                              ep->fifo_elem_size);
    elem->flags      = 0;
    elem->am_id      = id;
    elem->length     = (uint16_t)payload_total;
    elem->generation = ep->expected_generation;
    elem->header     = header;
    if (length > 0) {
        memcpy(elem + 1, payload, length);
    }

    ucs_memory_bus_store_fence();
    ep->tx_index      = index + 1;
    ep->tx_ctl->head  = ep->tx_index;

    UCT_TL_EP_STAT_OP(&ep->super, AM, SHORT, payload_total);
    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, id,
                       &elem->header, payload_total, "TX: AM_SHORT");
    return UCS_OK;
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
    uint32_t                 index;
    size_t                   length;
    ucs_status_t             status;

    (void)flags;
    UCT_CHECK_AM_ID(id);

    status = uct_obmm_ep_reserve_slot(ep, &index);
    if (status != UCS_OK) {
        return status;
    }

    elem = uct_obmm_lane_elem(ep->tx_elems, index, ep->fifo_mask,
                              ep->fifo_elem_size);
    desc = uct_obmm_lane_desc(ep->tx_descs, index, ep->fifo_mask,
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
    elem->flags      = UCT_OBMM_MAILBOX_ELEM_FLAG_BCOPY;

    ucs_memory_bus_store_fence();
    ep->tx_index     = index + 1;
    ep->tx_ctl->head = ep->tx_index;

    UCT_TL_EP_STAT_OP(&ep->super, AM, BCOPY, length);
    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, id,
                       desc, length, "TX: AM_BCOPY");
    return (ssize_t)length;
}


ucs_status_t uct_obmm_ep_pending_add(uct_ep_h tl_ep, uct_pending_req_t *n,
                                     unsigned flags)
{
    uct_obmm_ep_t    *ep    = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    uct_obmm_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_obmm_iface_t);

    (void)flags;

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
