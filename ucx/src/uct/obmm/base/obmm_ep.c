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
#include "obmm_block.h"
#include "obmm_fifo.h"
#include "obmm_atomic.h"

#include <uct/api/v2/uct_v2.h>
#include <uct/base/uct_log.h>
#include <uct/base/uct_iface.h>
#include <ucs/arch/atomic.h>
#include <ucs/arch/cpu.h>
#include <ucs/debug/assert.h>
#include <ucs/debug/log.h>
#include <ucs/debug/memtrack_int.h>
#include <ucs/sys/ptr_arith.h>

#include <inttypes.h>
#include <string.h>


static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_reserve_elem(uct_obmm_ep_t *ep, uint64_t *head_p);

static UCS_F_ALWAYS_INLINE void uct_obmm_ep_load_fence(uct_obmm_ep_t *ep)
{
    (void)ep;
    ucs_memory_bus_load_fence();
}

static UCS_F_ALWAYS_INLINE void uct_obmm_ep_store_fence(uct_obmm_ep_t *ep)
{
    (void)ep;
    ucs_memory_bus_store_fence();
}


static void uct_obmm_ep_close_peer_region(uct_obmm_ep_t *ep)
{
    if (ep->peer_region_opened) {
        uct_obmm_region_close(&ep->peer_region_storage);
        ep->peer_region_opened = 0;
    }
}


static UCS_CLASS_INIT_FUNC(uct_obmm_ep_t, const uct_ep_params_t *params)
{
    uct_obmm_iface_t             *iface = ucs_derived_of(params->iface,
                                                         uct_obmm_iface_t);
    const uct_obmm_device_addr_t *daddr;
    const uct_obmm_region_addr_t *peer_addr;
    uct_obmm_dev_info_t           peer_info;
    uct_obmm_region_t            *region;
    uct_obmm_block_t              peer_block;
    size_t                        peer_stride;
    int                           use_rx_region;
    ucs_status_t                  status;

    self->base_initialized      = 0;
    self->arb_group_initialized = 0;
    self->peer_region_opened    = 0;
    self->peer_region_storage.fd = -1;

    UCT_CHECK_PARAM(params->field_mask & UCT_EP_PARAM_FIELD_DEV_ADDR,
                    "UCT_EP_PARAM_FIELD_DEV_ADDR is not defined");
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);
    self->base_initialized = 1;

    ucs_arbiter_group_init(&self->arb_group);
    self->arb_group_initialized = 1;

    daddr = (const uct_obmm_device_addr_t*)params->dev_addr;
    if (daddr == NULL) {
        ucs_error("obmm: ep_create missing peer device address");
        return UCS_ERR_INVALID_PARAM;
    }

    peer_stride = uct_obmm_fifo_stride(iface->fifo_size,
                                       iface->fifo_elem_size);

    status = uct_obmm_iface_resolve_peer(iface, daddr, &peer_info,
                                         &use_rx_region);
    if (status != UCS_OK) {
        ucs_error("obmm: ep_create cannot resolve shmdev for peer "
                  "dcna=0x%lx deid=0x%x region_id=0x%x",
                  (unsigned long)daddr->primary.exporter_dcna,
                  daddr->primary.exporter_deid,
                  daddr->primary.region_id);
        return status;
    }

    if (use_rx_region) {
        region = iface->rx.region;
    } else {
        status = uct_obmm_region_open(&peer_info,
                                      &self->peer_region_storage);
        if (status != UCS_OK) {
            ucs_error("obmm: ep_create failed to map peer memid=%" PRIu64
                      ": %s", peer_info.memid, ucs_status_string(status));
            return status;
        }
        self->peer_region_opened = 1;
        region = &self->peer_region_storage;
    }

    status = uct_obmm_block_open(region->base, region->length,
                                 peer_stride, &peer_block);
    if (status != UCS_OK) {
        ucs_error("obmm: failed to open peer FIFO block: %s",
                  ucs_status_string(status));
        uct_obmm_ep_close_peer_region(self);
        return status;
    }

    peer_addr = &daddr->primary;

    self->peer_ctl            = peer_block.ctl;
    self->peer_elems          = peer_block.elems;
    self->peer_block_base     = region->base;
    self->peer_block_length   = region->length;
    self->cached_tail         = self->peer_ctl->tail;
    uct_obmm_ep_load_fence(self);
    self->fifo_size           = iface->fifo_size;
    self->fifo_mask           = iface->fifo_mask;
    self->fifo_elem_size      = iface->fifo_elem_size;
    self->bcopy_seg_size      = iface->bcopy_seg_size;
    self->peer_dcna           = peer_addr->exporter_dcna;
    self->peer_deid           = peer_addr->exporter_deid;
    self->peer_region_id      = peer_addr->region_id;
    return UCS_OK;
}


static UCS_CLASS_CLEANUP_FUNC(uct_obmm_ep_t)
{
    /* Drain any UCP requests still parked on this ep's arbiter group
     * before the iface tears down its arbiter. mm follows the same
     * order (mm_ep.c:217). */
    if (self->base_initialized && self->arb_group_initialized) {
        uct_obmm_ep_pending_purge(&self->super.super, NULL, NULL);
    }
    uct_obmm_ep_close_peer_region(self);
}


UCS_CLASS_DEFINE(uct_obmm_ep_t, uct_base_ep_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_obmm_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_obmm_ep_t, uct_ep_t);


int uct_obmm_ep_is_connected(const uct_ep_h tl_ep,
                             const uct_ep_is_connected_params_t *params)
{
    const uct_obmm_ep_t          *ep = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    const uct_obmm_device_addr_t *daddr;
    const uct_obmm_region_addr_t *peer_addr;

    if (!uct_base_ep_is_connected(tl_ep, params)) {
        return 0;
    }

    UCT_EP_IS_CONNECTED_CHECK_DEV_ADDR(params);
    daddr = (const uct_obmm_device_addr_t*)params->device_addr;
    if (daddr == NULL) {
        return 0;
    }

    peer_addr = &daddr->primary;
    return (peer_addr->exporter_dcna == ep->peer_dcna) &&
           (peer_addr->exporter_deid == ep->peer_deid) &&
           (peer_addr->region_id == ep->peer_region_id);
}


ucs_status_t uct_obmm_ep_query(uct_ep_h tl_ep, uct_ep_attr_t *ep_attr)
{
    (void)tl_ep;

    if (ep_attr->field_mask == 0) {
        return UCS_OK;
    }

    return UCS_ERR_UNSUPPORTED;
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

    status = uct_obmm_ep_reserve_elem(ep, &head);
    if (status != UCS_OK) {
        return status;
    }

    elem             = uct_obmm_fifo_elem_at(ep->peer_elems, head,
                                             ep->fifo_mask,
                                             ep->fifo_elem_size);
    elem->am_id      = id;
    elem->length     = (uint32_t)payload_total;
    elem->header     = header;
    short_data       = uct_obmm_fifo_elem_short_data(elem);
    if (length > 0) {
        memcpy(UCS_PTR_BYTE_OFFSET(short_data, sizeof(header)), payload,
               length);
    }

    owner_bit = (head & ep->fifo_size) ? 0u :
                                        UCT_OBMM_FIFO_ELEM_FLAG_OWNER;
    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, id,
                       short_data, payload_total, "TX: AM_SHORT_FIFO");
    uct_obmm_ep_store_fence(ep);
    elem->flags = owner_bit;

    UCT_TL_EP_STAT_OP(&ep->super, AM, SHORT, payload_total);
    return UCS_OK;
}


/* Reserve one element in the peer's FIFO, returning the head index that was
 * claimed. Use CAS (not FAA): if an FAA claim succeeds and the FIFO then turns
 * out to be full, the head bump cannot be rolled back and would leave a
 * permanent hole. On aarch64 NC mappings this must be an explicit LSE CAS, not
 * a compiler-default LL/SC atomic. */
static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_reserve_elem(uct_obmm_ep_t *ep, uint64_t *head_p)
{
    uint64_t head;

    for (;;) {
        head = ep->peer_ctl->head;

        if ((head - ep->cached_tail) >= ep->fifo_size) {
            ep->cached_tail = ep->peer_ctl->tail;
            /* Order the tail value which authorizes slot reuse before any
             * later load of that slot's replacement descriptor offset. */
            uct_obmm_ep_load_fence(ep);
            if ((head - ep->cached_tail) >= ep->fifo_size) {
                UCS_STATS_UPDATE_COUNTER(ep->super.stats, UCT_EP_STAT_NO_RES,
                                         1);
                return UCS_ERR_NO_RESOURCE;
            }
        }

        if (uct_obmm_atomic_cswap64(&ep->peer_ctl->head, head,
                                    head + 1) == head) {
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
    void                    *data;
    uint64_t                 desc_offset;
    uint64_t                 min_desc_offset;
    uint64_t                 head;
    size_t                   length;
    uint8_t                  owner_bit;
    ucs_status_t             status;

    /* flags (UCT_SEND_FLAG_PEER_CHECK etc.) are ignored: this transport
     * does not advertise EP_CHECK / keepalive in v1. */
    (void)flags;

    UCT_CHECK_AM_ID(id);

    status = uct_obmm_ep_reserve_elem(ep, &head);
    if (status != UCS_OK) {
        return status;
    }

    elem = uct_obmm_fifo_elem_at(ep->peer_elems, head, ep->fifo_mask,
                                 ep->fifo_elem_size);

    /* Pair with the receiver's replacement-offset store + full fence + tail
     * publication before dereferencing this reusable FIFO slot. */
    uct_obmm_ep_load_fence(ep);
    desc_offset = elem->desc_offset;
    min_desc_offset = (uint64_t)UCS_PTR_BYTE_DIFF(ep->peer_block_base,
                                                  ep->peer_elems) +
                      (size_t)ep->fifo_size * ep->fifo_elem_size;
    if (ucs_unlikely((desc_offset < min_desc_offset) ||
                     (ep->bcopy_seg_size > ep->peer_block_length) ||
                     (desc_offset > (ep->peer_block_length -
                                     ep->bcopy_seg_size)))) {
        ucs_fatal("obmm: invalid peer bcopy descriptor offset %" PRIu64
                  " (min=%" PRIu64 " block=%zu payload=%u)", desc_offset,
                  min_desc_offset, ep->peer_block_length,
                  ep->bcopy_seg_size);
    }
    data = UCS_PTR_BYTE_OFFSET(ep->peer_block_base, (size_t)desc_offset);

    /* UCP limits the pack length from cap.am.max_bcopy before calling UCT.
     * Keep the standard parameter-check-build diagnostic for that contract. */
    length = pack_cb(data, arg);
    UCT_CHECK_LENGTH(length, 0, ep->bcopy_seg_size, "am_bcopy");

    elem->am_id      = id;
    elem->length     = (uint32_t)length;

    owner_bit = (head & ep->fifo_size) ? 0u :
                                        UCT_OBMM_FIFO_ELEM_FLAG_OWNER;

    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, id,
                       data, length, "TX: AM_BCOPY");

    /* Release barrier: orders the shared-data payload writes AND elem header
     * writes BEFORE the flags publish. Receiver pairs with the matching load
     * fence after observing the flags byte. */
    uct_obmm_ep_store_fence(ep);
    elem->flags = owner_bit | UCT_OBMM_FIFO_ELEM_FLAG_BCOPY;

    UCT_TL_EP_STAT_OP(&ep->super, AM, BCOPY, length);
    return (ssize_t)length;
}


ucs_status_t uct_obmm_ep_flush(uct_ep_h tl_ep, unsigned flags,
                               uct_completion_t *comp)
{
    uct_obmm_ep_t *ep = ucs_derived_of(tl_ep, uct_obmm_ep_t);

    (void)ep;
    (void)flags;
    (void)comp;

    UCT_TL_EP_STAT_FLUSH(&ep->super);
    return UCS_OK;
}


/* Returns true iff the peer's FIFO has at least one free element, refreshing
 * cached_tail with a load fence before declaring "full". Mirrors the resource
 * check used by mm in pending_add. */
static UCS_F_ALWAYS_INLINE int
uct_obmm_ep_has_tx_resource(uct_obmm_ep_t *ep)
{
    uint64_t head = ep->peer_ctl->head;

    if ((head - ep->cached_tail) < ep->fifo_size) {
        return 1;
    }
    ep->cached_tail = ep->peer_ctl->tail;
    uct_obmm_ep_load_fence(ep);
    return (head - ep->cached_tail) < ep->fifo_size;
}


ucs_status_t uct_obmm_ep_pending_add(uct_ep_h tl_ep, uct_pending_req_t *n,
                                     unsigned flags)
{
    uct_obmm_ep_t    *ep    = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    uct_obmm_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_obmm_iface_t);

    (void)flags;

    /* NO_RESOURCE here means FIFO backpressure. Only tell UCP to retry
     * directly when the ep has no older queued requests; otherwise keep
     * FIFO order by queueing behind the existing pending group. */
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
    uct_obmm_ep_t     *ep    = ucs_container_of(group, uct_obmm_ep_t,
                                                arb_group);
    uct_obmm_iface_t  *iface = ucs_derived_of(ep->super.super.iface,
                                              uct_obmm_iface_t);
    unsigned          *count = (unsigned*)arg;
    uct_pending_req_t *req;
    ucs_status_t       status;

    (void)arbiter;

    if (*count >= iface->pending_quota) {
        return UCS_ARBITER_CB_RESULT_STOP;
    }

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

    (void)arbiter;

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
