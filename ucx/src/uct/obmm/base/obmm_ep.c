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

#include <uct/api/v2/uct_v2.h>
#include <uct/base/uct_log.h>
#include <uct/base/uct_iface.h>
#include <ucs/arch/atomic.h>
#include <ucs/arch/cpu.h>
#include <ucs/debug/log.h>
#include <ucs/debug/memtrack_int.h>
#include <ucs/sys/ptr_arith.h>

#include <string.h>


static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_reserve_slot(uct_obmm_ep_t *ep, uint64_t *head_p);

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


static ucs_status_t
uct_obmm_ep_validate_peer_addr(const uct_obmm_iface_addr_t *iaddr,
                               size_t *peer_stride_p)
{
    size_t peer_stride;

    if (iaddr->wire_format != UCT_OBMM_WIRE_FORMAT_CURRENT) {
        ucs_error("obmm: peer UCT ABI wire=%u differs from local wire=%u",
                  iaddr->wire_format, UCT_OBMM_WIRE_FORMAT_CURRENT);
        return UCS_ERR_UNREACHABLE;
    }
    if (iaddr->slot_index == UINT32_MAX) {
        ucs_error("obmm: invalid peer slot_index %u", iaddr->slot_index);
        return UCS_ERR_INVALID_PARAM;
    }

    if ((iaddr->fifo_size == 0) ||
        ((iaddr->fifo_size & (iaddr->fifo_size - 1u)) != 0)) {
        ucs_error("obmm: invalid peer FIFO_SIZE %u", iaddr->fifo_size);
        return UCS_ERR_INVALID_PARAM;
    }

    if (iaddr->fifo_elem_size <= uct_obmm_fifo_bcopy_data_offset()) {
        ucs_error("obmm: invalid peer FIFO_ELEM_SIZE %u", iaddr->fifo_elem_size);
        return UCS_ERR_INVALID_PARAM;
    }

    if ((iaddr->bcopy_seg_size == 0) ||
        (iaddr->bcopy_seg_size >
         uct_obmm_fifo_max_bcopy(iaddr->fifo_elem_size))) {
        ucs_error("obmm: invalid peer BCOPY_SEG_SIZE %u "
                  "(FIFO_ELEM_SIZE=%u max=%u)",
                  iaddr->bcopy_seg_size, iaddr->fifo_elem_size,
                  uct_obmm_fifo_max_bcopy(iaddr->fifo_elem_size));
        return UCS_ERR_INVALID_PARAM;
    }

    peer_stride = uct_obmm_slot_stride(iaddr->fifo_size,
                                       iaddr->fifo_elem_size);
    if (peer_stride > UINT32_MAX) {
        ucs_error("obmm: peer slot stride %zu exceeds uint32_t "
                  "(fifo=%u elem=%u)",
                  peer_stride, iaddr->fifo_size, iaddr->fifo_elem_size);
        return UCS_ERR_INVALID_PARAM;
    }

    *peer_stride_p = peer_stride;
    return UCS_OK;
}


static UCS_CLASS_INIT_FUNC(uct_obmm_ep_t, const uct_ep_params_t *params)
{
    uct_obmm_iface_t             *iface = ucs_derived_of(params->iface,
                                                         uct_obmm_iface_t);
    const uct_obmm_device_addr_t *daddr;
    const uct_obmm_iface_addr_t  *iaddr;
    const uct_obmm_region_addr_t *peer_addr;
    uct_obmm_region_t            *region;
    uct_obmm_pool_t               peer_pool;
    void                         *peer_slot;
    size_t                        peer_stride;
    uint32_t                      slot_index;
    ucs_status_t                  status;

    self->base_initialized      = 0;
    self->arb_group_initialized = 0;

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);
    self->base_initialized = 1;

    ucs_arbiter_group_init(&self->arb_group);
    self->arb_group_initialized = 1;

    daddr = (const uct_obmm_device_addr_t*)params->dev_addr;
    iaddr = (const uct_obmm_iface_addr_t*)params->iface_addr;
    if ((params->field_mask & UCT_EP_PARAM_FIELD_IFACE_ADDR_LENGTH) &&
        (params->iface_addr_length < sizeof(*iaddr))) {
        ucs_error("obmm: iface address too short: peer=%zu local=%zu",
                  params->iface_addr_length, sizeof(*iaddr));
        return UCS_ERR_UNREACHABLE;
    }

    status = uct_obmm_ep_validate_peer_addr(iaddr, &peer_stride);
    if (status != UCS_OK) {
        return status;
    }

    region = uct_obmm_iface_resolve_peer_region(iface, daddr, iaddr,
                                                &slot_index);
    if (region == NULL) {
        ucs_error("obmm: ep_create cannot find mapped region for peer "
                  "dcna=0x%lx deid=0x%lx:0x%lx region_id=0x%x",
                  (unsigned long)daddr->primary.exporter_dcna,
                  (unsigned long)daddr->primary.exporter_deid_hi,
                  (unsigned long)daddr->primary.exporter_deid_lo,
                  daddr->primary.region_id);
        return UCS_ERR_UNREACHABLE;
    }

    status = uct_obmm_pool_open(region->base, region->length,
                                UCT_OBMM_POOL_SLOT_COUNT,
                                (uint32_t)peer_stride, &peer_pool);
    if (status != UCS_OK) {
        ucs_error("obmm: failed to open peer pool: %s",
                  ucs_status_string(status));
        return status;
    }

    if (slot_index >= peer_pool.slot_count) {
        ucs_error("obmm: peer slot_index %u out of range (slot_count=%u)",
                  slot_index, peer_pool.slot_count);
        return UCS_ERR_INVALID_PARAM;
    }

    peer_slot = uct_obmm_pool_slot_ptr(&peer_pool, slot_index);
    peer_addr = &daddr->primary;

    self->peer_ctl            = uct_obmm_slot_ctl(peer_slot);
    self->peer_elems          = uct_obmm_slot_elems(peer_slot);
    self->cached_tail         = self->peer_ctl->tail;
    self->fifo_size           = iaddr->fifo_size;
    self->fifo_mask           = iaddr->fifo_size - 1u;
    self->fifo_elem_size      = iaddr->fifo_elem_size;
    self->bcopy_seg_size      = iaddr->bcopy_seg_size;
    self->peer_dcna           = peer_addr->exporter_dcna;
    self->peer_deid_hi        = peer_addr->exporter_deid_hi;
    self->peer_deid_lo        = peer_addr->exporter_deid_lo;
    self->peer_region_id      = peer_addr->region_id;
    self->peer_slot_index     = slot_index;
    self->peer_pid            = iaddr->pid;
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
    const uct_obmm_region_addr_t *peer_addr;

    if (!uct_base_ep_is_connected(tl_ep, params)) {
        return 0;
    }

    daddr = (const uct_obmm_device_addr_t*)params->device_addr;
    iaddr = (const uct_obmm_iface_addr_t*)params->iface_addr;
    if ((daddr == NULL) || (iaddr == NULL)) {
        return 0;
    }

    peer_addr = &daddr->primary;
    return (peer_addr->exporter_dcna == ep->peer_dcna) &&
           (peer_addr->exporter_deid_hi == ep->peer_deid_hi) &&
           (peer_addr->exporter_deid_lo == ep->peer_deid_lo) &&
           (peer_addr->region_id == ep->peer_region_id) &&
           (iaddr->wire_format == UCT_OBMM_WIRE_FORMAT_CURRENT) &&
           (iaddr->fifo_size == ep->fifo_size) &&
           (iaddr->fifo_elem_size == ep->fifo_elem_size) &&
           (iaddr->bcopy_seg_size == ep->bcopy_seg_size) &&
           (iaddr->slot_index == ep->peer_slot_index);
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

    status = uct_obmm_ep_reserve_slot(ep, &head);
    if (status != UCS_OK) {
        return status;
    }

    elem             = uct_obmm_slot_elem(ep->peer_elems, head, ep->fifo_mask,
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


/* Reserve one slot in the peer's FIFO, returning the head index that was
 * claimed. Use CAS (not FAA): if an FAA claim succeeds and the FIFO then turns
 * out to be full, the head bump cannot be rolled back and would leave a
 * permanent hole. On aarch64 NC mappings this must be an explicit LSE CAS, not
 * a compiler-default LL/SC atomic. */
static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_reserve_slot(uct_obmm_ep_t *ep, uint64_t *head_p)
{
    uint64_t head;

    for (;;) {
        head = ep->peer_ctl->head;

        if ((head - ep->cached_tail) >= ep->fifo_size) {
            uct_obmm_ep_load_fence(ep);
            ep->cached_tail = ep->peer_ctl->tail;
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
    data = uct_obmm_fifo_elem_bcopy_data(elem);

    /* pack_cb writes pack_cb_ret bytes directly into the shared FIFO data
     * area. UCP guarantees pack_cb_ret <= cap.am.max_bcopy, which we set
     * to bcopy_seg_size. The assert catches buggy direct UCT users in
     * debug builds; production safety relies on the iface cap contract. */
    length = pack_cb(data, arg);
    ucs_assertv(length <= ep->bcopy_seg_size,
                "obmm: pack_cb returned %zu > bcopy_seg_size=%u",
                length, ep->bcopy_seg_size);
    ucs_assertv(length <= uct_obmm_fifo_max_bcopy(ep->fifo_elem_size),
                "obmm: pack_cb returned %zu > fifo data capacity=%u",
                length, uct_obmm_fifo_max_bcopy(ep->fifo_elem_size));

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

    (void)flags;
    (void)comp;

    UCT_TL_EP_STAT_FLUSH(&ep->super);
    return UCS_OK;
}


/* Returns true iff the peer's FIFO has at least one free slot, refreshing
 * cached_tail with a load fence before declaring "full". Mirrors the resource
 * check used by mm in pending_add. */
static UCS_F_ALWAYS_INLINE int
uct_obmm_ep_has_tx_resource(uct_obmm_ep_t *ep)
{
    uint64_t head = ep->peer_ctl->head;

    if ((head - ep->cached_tail) < ep->fifo_size) {
        return 1;
    }
    uct_obmm_ep_load_fence(ep);
    ep->cached_tail = ep->peer_ctl->tail;
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
