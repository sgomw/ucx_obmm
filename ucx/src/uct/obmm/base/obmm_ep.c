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


typedef enum uct_obmm_send_op {
    UCT_OBMM_SEND_AM_SHORT,
    UCT_OBMM_SEND_AM_BCOPY
} uct_obmm_send_op_t;


static UCS_F_ALWAYS_INLINE uint8_t
uct_obmm_ep_fifo_bank(uct_obmm_iface_t *iface,
                      const uct_obmm_device_addr_t *daddr)
{
    return ((iface->region->info.exporter_dcna == daddr->exporter_dcna) &&
            (iface->region->info.exporter_deid.hi == daddr->exporter_deid_hi) &&
            (iface->region->info.exporter_deid.lo == daddr->exporter_deid_lo)) ?
           UCT_OBMM_FIFO_BANK_LOCAL : UCT_OBMM_FIFO_BANK_REMOTE;
}


static UCS_F_ALWAYS_INLINE void
uct_obmm_ep_update_cached_tail(uct_obmm_ep_t *ep)
{
    ucs_memory_bus_load_fence();
    ep->cached_tail = ep->fifo_ctl->tail;
}


static UCS_F_ALWAYS_INLINE int
uct_obmm_ep_has_tx_resource(uct_obmm_ep_t *ep)
{
    uint64_t head = ep->fifo_ctl->head;

    if (uct_obmm_fifo_has_space(head, ep->cached_tail, ep->fifo_size)) {
        return 1;
    }

    uct_obmm_ep_update_cached_tail(ep);
    return uct_obmm_fifo_has_space(head, ep->cached_tail, ep->fifo_size);
}


static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_no_resources_handle(uct_obmm_ep_t *ep)
{
    UCS_STATS_UPDATE_COUNTER(ep->super.stats, UCT_EP_STAT_NO_RES, 1);
    return UCS_ERR_NO_RESOURCE;
}


static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_get_remote_elem(uct_obmm_ep_t *ep, uint64_t head,
                            uct_obmm_fifo_element_t **elem_p)
{
    uint64_t new_head = head + 1;
    uint64_t prev_head;

    *elem_p = uct_obmm_shard_elem(ep->fifo_elems, head, ep->fifo_mask,
                                  ep->fifo_elem_size);
    prev_head = uct_obmm_atomic_cswap64(&ep->fifo_ctl->head, head, new_head);
    if (prev_head != head) {
        return UCS_ERR_NO_RESOURCE;
    }

    return UCS_OK;
}


static UCS_F_ALWAYS_INLINE ssize_t
uct_obmm_ep_am_common_send(uct_obmm_send_op_t send_op, uct_obmm_ep_t *ep,
                           uct_obmm_iface_t *iface, uint8_t am_id,
                           size_t length, uint64_t header,
                           const void *payload, uct_pack_callback_t pack_cb,
                           void *arg)
{
    uct_obmm_fifo_element_t *elem;
    uint8_t                  elem_flags;
    uint64_t                 head;
    void                    *desc;
    ucs_status_t             status;

    UCT_CHECK_AM_ID(am_id);

retry:
    if (!ucs_arbiter_group_is_empty(&ep->arb_group)) {
        return uct_obmm_ep_no_resources_handle(ep);
    }

    head = ep->fifo_ctl->head;
    if (!uct_obmm_fifo_has_space(head, ep->cached_tail, ep->fifo_size)) {
        uct_obmm_ep_update_cached_tail(ep);
        if (!uct_obmm_fifo_has_space(head, ep->cached_tail, ep->fifo_size)) {
            return uct_obmm_ep_no_resources_handle(ep);
        }
    }

    status = uct_obmm_ep_get_remote_elem(ep, head, &elem);
    if (status != UCS_OK) {
        goto retry;
    }

    switch (send_op) {
    case UCT_OBMM_SEND_AM_SHORT:
        elem_flags      = 0;
        elem->am_id     = am_id;
        elem->length    = (uint16_t)(sizeof(header) + length);
        elem->generation = ep->expected_generation;
        elem->header    = header;
        if (length > 0) {
            memcpy(elem + 1, payload, length);
        }
        UCT_TL_EP_STAT_OP(&ep->super, AM, SHORT, sizeof(header) + length);
        break;
    case UCT_OBMM_SEND_AM_BCOPY:
        desc = uct_obmm_shard_desc(ep->fifo_descs, head, ep->fifo_mask,
                                   ep->bcopy_seg_size);
        length = pack_cb(desc, arg);
        ucs_assertv(length <= ep->bcopy_seg_size,
                    "obmm: pack_cb returned %zu > bcopy_seg_size=%u",
                    length, ep->bcopy_seg_size);
        ucs_assertv(length <= UINT16_MAX,
                    "obmm: pack_cb returned %zu > UINT16_MAX", length);

        elem_flags       = UCT_OBMM_FIFO_ELEM_FLAG_BCOPY;
        elem->am_id      = am_id;
        elem->length     = (uint16_t)length;
        elem->generation = ep->expected_generation;
        elem->header     = 0;
        UCT_TL_EP_STAT_OP(&ep->super, AM, BCOPY, length);
        break;
    default:
        return UCS_ERR_INVALID_PARAM;
    }

    ucs_memory_bus_store_fence();
    elem->flags = elem_flags | uct_obmm_fifo_owner_bit(head, ep->fifo_size);

    switch (send_op) {
    case UCT_OBMM_SEND_AM_SHORT:
        uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, am_id,
                           &elem->header, sizeof(header) + length,
                           "TX: AM_SHORT");
        return UCS_OK;
    case UCT_OBMM_SEND_AM_BCOPY:
        uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, am_id,
                           desc, length, "TX: AM_BCOPY");
        return (ssize_t)length;
    default:
        return UCS_ERR_INVALID_PARAM;
    }
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
    uct_obmm_slot_meta_t         *meta;
    void                         *peer_slot;
    void                         *shard_base;
    ucs_status_t                  status;
    uct_obmm_eid_t                eid;

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);

    ucs_arbiter_group_init(&self->arb_group);

    daddr = (const uct_obmm_device_addr_t*)params->dev_addr;
    iaddr = (const uct_obmm_iface_addr_t*)params->iface_addr;

    if ((iaddr->layout != UCT_OBMM_FIFO_LAYOUT_ATOMIC_SHARDED) ||
        (iaddr->shard_count != iface->shard_count) ||
        (iaddr->fifo_size != iface->fifo_size) ||
        (iaddr->fifo_elem_size != iface->fifo_elem_size) ||
        (iaddr->bcopy_seg_size != iface->bcopy_seg_size)) {
        ucs_error("obmm: peer geometry/layout differs from local "
                  "(peer layout=%u shards=%u fifo=%u elem=%u seg=%u, "
                  "local layout=%u shards=%u fifo=%u elem=%u seg=%u); "
                  "ep_create rejected",
                  iaddr->layout, iaddr->shard_count, iaddr->fifo_size,
                  iaddr->fifo_elem_size, iaddr->bcopy_seg_size,
                  UCT_OBMM_FIFO_LAYOUT_ATOMIC_SHARDED, iface->shard_count,
                  iface->fifo_size, iface->fifo_elem_size,
                  iface->bcopy_seg_size);
        return UCS_ERR_UNREACHABLE;
    }

    eid.hi = daddr->exporter_deid_hi;
    eid.lo = daddr->exporter_deid_lo;

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
        uct_obmm_slot_stride(iaddr->shard_count, iaddr->fifo_size,
                             iaddr->fifo_elem_size, iaddr->bcopy_seg_size)) {
        ucs_error("obmm: peer pool slot_size %u inconsistent with iface_addr "
                  "geometry (shards=%u fifo=%u elem=%u seg=%u)",
                  peer_pool.slot_size, iaddr->shard_count, iaddr->fifo_size,
                  iaddr->fifo_elem_size, iaddr->bcopy_seg_size);
        return UCS_ERR_INVALID_PARAM;
    }

    meta = &peer_pool.meta[iaddr->slot_index];
    ucs_memory_bus_load_fence();
    if ((meta->state != UCT_OBMM_SLOT_STATE_IN_USE) ||
        (meta->generation != iaddr->generation)) {
        ucs_error("obmm: peer slot %u generation/state changed before ep_create "
                  "(state=%u generation=%u expected_generation=%u)",
                  iaddr->slot_index, meta->state, meta->generation,
                  iaddr->generation);
        return UCS_ERR_UNREACHABLE;
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
    self->shard_count         = iaddr->shard_count;
    self->shard_index         = iface->slot_index & (iaddr->shard_count - 1u);
    self->fifo_bank           = uct_obmm_ep_fifo_bank(iface, daddr);

    peer_slot = uct_obmm_pool_slot_ptr(&peer_pool, iaddr->slot_index);
    shard_base = uct_obmm_slot_shard(peer_slot, self->fifo_bank,
                                     self->shard_index, iaddr->shard_count,
                                     iaddr->fifo_size, iaddr->fifo_elem_size,
                                     iaddr->bcopy_seg_size);
    self->fifo_ctl   = uct_obmm_shard_ctl(shard_base);
    self->fifo_elems = uct_obmm_shard_elems(shard_base);
    self->fifo_descs = uct_obmm_shard_descs(shard_base, iaddr->fifo_size,
                                            iaddr->fifo_elem_size);
    ucs_memory_bus_load_fence();
    if (self->fifo_ctl->receiver_generation != self->expected_generation) {
        ucs_error("obmm: peer shard generation changed before ep_create "
                  "(slot=%u shard=%u bank=%u shard_generation=%u "
                  "expected_generation=%u)",
                  self->peer_slot_index, self->shard_index, self->fifo_bank,
                  self->fifo_ctl->receiver_generation,
                  self->expected_generation);
        return UCS_ERR_UNREACHABLE;
    }
    self->cached_tail = self->fifo_ctl->tail;

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
           (iaddr->slot_index == ep->peer_slot_index) &&
           (iaddr->generation == ep->expected_generation) &&
           (iaddr->layout == UCT_OBMM_FIFO_LAYOUT_ATOMIC_SHARDED) &&
           (iaddr->shard_count == ep->shard_count);
}


ucs_status_t uct_obmm_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t header,
                                  const void *payload, unsigned length)
{
    uct_obmm_ep_t    *ep    = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    uct_obmm_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_obmm_iface_t);
    size_t            total = sizeof(header) + length;

    UCT_CHECK_LENGTH(total, 0,
                     ep->fifo_elem_size - sizeof(uct_obmm_fifo_element_t),
                     "am_short");

    return (ucs_status_t)uct_obmm_ep_am_common_send(UCT_OBMM_SEND_AM_SHORT, ep,
                                                    iface, id, length, header,
                                                    payload, NULL, NULL);
}


ssize_t uct_obmm_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                             uct_pack_callback_t pack_cb, void *arg,
                             unsigned flags)
{
    uct_obmm_ep_t    *ep    = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    uct_obmm_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_obmm_iface_t);

    (void)flags;
    return uct_obmm_ep_am_common_send(UCT_OBMM_SEND_AM_BCOPY, ep, iface, id, 0,
                                      0, NULL, pack_cb, arg);
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

    uct_obmm_ep_update_cached_tail(ep);
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
