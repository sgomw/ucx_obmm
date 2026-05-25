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
#include <ucs/time/time.h>

#include <unistd.h>
#include <string.h>

static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_reserve_slot(uct_obmm_ep_t *ep, uint64_t *head_p);

static UCS_F_ALWAYS_INLINE void
uct_obmm_ep_short_lane_activate(volatile uint64_t *active_mask_p,
                                unsigned lane_index)
{
    uint64_t bit = 1ull << lane_index;
    uint64_t mask;

    for (;;) {
        mask = *active_mask_p;
        if (mask & bit) {
            return;
        }

        if (uct_obmm_atomic_bool_cswap64(active_mask_p, mask, mask | bit)) {
            return;
        }
    }
}


static UCS_F_ALWAYS_INLINE int
uct_obmm_ep_short_lane_is_local_sender(const uct_obmm_iface_t             *iface,
                                       const uct_obmm_device_addr_t       *daddr)
{
    return (iface->region->info.exporter_dcna == daddr->exporter_dcna) &&
           (iface->region->info.exporter_deid.hi == daddr->exporter_deid_hi) &&
           (iface->region->info.exporter_deid.lo == daddr->exporter_deid_lo);
}


static UCS_F_ALWAYS_INLINE void
uct_obmm_ep_init_short_lane(uct_obmm_ep_t                  *ep,
                             const uct_obmm_iface_t         *iface,
                             const uct_obmm_device_addr_t   *daddr,
                             void                           *peer_slot)
{
    uct_obmm_short_lane_t *lane;
    volatile uint64_t     *active_mask_p;
    unsigned               lane_index;

    lane_index = iface->slot_index;
    if (!uct_obmm_ep_short_lane_is_local_sender(iface, daddr)) {
        lane_index += UCT_OBMM_POOL_SLOT_COUNT;
    }

    lane          = uct_obmm_slot_short_lane(peer_slot, lane_index);
    active_mask_p = uct_obmm_slot_short_active_mask(peer_slot);
    if ((lane->meta.sender_slot_index != iface->slot_index) ||
        (lane->meta.sender_generation != iface->generation) ||
        (lane->meta.sender_pid != (uint32_t)getpid())) {
        lane->meta.sender_slot_index = iface->slot_index;
        lane->meta.sender_generation = iface->generation;
        lane->meta.sender_pid        = (uint32_t)getpid();
        lane->ctl.head               = 0;
        lane->ctl.tail               = 0;
        ucs_memory_bus_store_fence();
    }
    ep->short_lane_active_mask_p = active_mask_p;
    ep->short_lane             = lane;
    ep->short_lane_index       = lane_index;
    ep->short_lane_head        = lane->ctl.head;
    ep->short_lane_cached_tail = lane->ctl.tail;
    ep->short_lane_active      = 0;
}


static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_am_short_spsc(uct_obmm_ep_t *ep, uct_obmm_iface_t *iface,
                          uint8_t id, uint64_t header, const void *payload,
                          unsigned length, size_t payload_total)
{
    uct_obmm_short_lane_t    *lane = ep->short_lane;
    uct_obmm_fifo_element_t  *elem;
    uint64_t                  head = ep->short_lane_head;
    ucs_time_t                total_start = 0;
    ucs_time_t                copy_start = 0;
    ucs_time_t                publish_start = 0;
    int                       short_perf_1b;

    if (!ep->short_lane_active) {
        uct_obmm_ep_short_lane_activate(ep->short_lane_active_mask_p,
                                        ep->short_lane_index);
        ep->short_lane_active = 1;
    }

    short_perf_1b = iface->short_perf_enable && (length == 1);
    if (short_perf_1b) {
        total_start = ucs_get_time();
    }
    if ((head - ep->short_lane_cached_tail) >= UCT_OBMM_SHORT_LANE_FIFO_SIZE) {
        ucs_memory_bus_load_fence();
        ep->short_lane_cached_tail = lane->ctl.tail;
        if ((head - ep->short_lane_cached_tail) >=
            UCT_OBMM_SHORT_LANE_FIFO_SIZE) {
            if (short_perf_1b) {
                iface->short_perf.tx_1b_nores++;
            }
            if (ucs_unlikely(iface->stats_enable)) {
                iface->baseline.tx_fifo_full++;
            }
            return UCS_ERR_NO_RESOURCE;
        }
    }

    if (short_perf_1b) {
        copy_start = ucs_get_time();
    }
    elem             = uct_obmm_short_lane_elem(lane, head);
    elem->flags      = 0;
    elem->am_id      = id;
    elem->length     = (uint16_t)payload_total;
    elem->generation = ep->expected_generation;
    elem->header     = header;
    if (length > 0) {
        memcpy(elem + 1, payload, length);
    }
    if (short_perf_1b) {
        iface->short_perf.tx_1b_copy_ticks += (ucs_get_time() - copy_start);
        publish_start = ucs_get_time();
    }

    ucs_memory_bus_store_fence();
    lane->ctl.head      = head + 1;
    ep->short_lane_head = head + 1;
    if (short_perf_1b) {
        iface->short_perf.tx_1b_publish_ticks += (ucs_get_time() - publish_start);
        iface->short_perf.tx_1b_total_ticks   += (ucs_get_time() - total_start);
        iface->short_perf.tx_1b_msgs++;
    }

    if (ucs_unlikely(iface->stats_enable)) {
        iface->baseline.tx_msgs++;
        iface->baseline.tx_bytes += payload_total;
        iface->baseline.tx_short_msgs++;
    }
    UCT_TL_EP_STAT_OP(&ep->super, AM, SHORT, payload_total);
    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, id,
                       &header, payload_total, "TX: AM_SHORT_SPSC");
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
    ucs_status_t                  status;

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);

    ucs_arbiter_group_init(&self->arb_group);

    daddr = (const uct_obmm_device_addr_t*)params->dev_addr;
    iaddr = (const uct_obmm_iface_addr_t*)params->iface_addr;

    /* Reject incompatible geometry. UCX wireup should already have filtered
     * this out via is_reachable_v2, but double-check. */
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
    uct_obmm_ep_init_short_lane(self, iface, daddr, peer_slot);
    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_obmm_ep_t)
{
    /* Drain any UCP requests still parked on this ep's arbiter group
     * before the iface tears down its arbiter. mm follows the same
     * order (mm_ep.c:217). */
    uct_obmm_ep_pending_purge(&self->super.super, NULL, NULL);
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

    UCT_CHECK_AM_ID(id);
    UCT_CHECK_LENGTH(payload_total, 0, uct_obmm_short_lane_max_short(),
                     "am_short");
    return uct_obmm_ep_am_short_spsc(ep, iface, id, header, payload, length,
                                     payload_total);
}


/* Reserve one slot in the peer's FIFO, returning the head index that was
 * claimed. Use CAS (not FAA): if an FAA claim succeeds and the FIFO then turns
 * out to be full, the head bump cannot be rolled back and would leave a
 * permanent hole. On aarch64 NC mappings this must be an explicit LSE CAS, not
 * a compiler-default LL/SC atomic. */
static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_reserve_slot(uct_obmm_ep_t *ep, uint64_t *head_p)
{
    uct_obmm_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                             uct_obmm_iface_t);
    uint64_t head;

    for (;;) {
        head = ep->peer_ctl->head;

        if ((head - ep->cached_tail) >= ep->fifo_size) {
            ucs_memory_bus_load_fence();
            ep->cached_tail = ep->peer_ctl->tail;
            if ((head - ep->cached_tail) >= ep->fifo_size) {
                if (ucs_unlikely(iface->stats_enable)) {
                    iface->baseline.tx_fifo_full++;
                }
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

        if (ucs_unlikely(iface->stats_enable)) {
            iface->baseline.tx_cas_retries++;
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
    void                    *desc;
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

    owner_bit = (head & ep->fifo_size) ? 0u :
                                        UCT_OBMM_FIFO_ELEM_FLAG_OWNER;

    /* Release barrier: orders the desc[N] payload writes AND elem header
     * writes BEFORE the flags publish. Receiver pairs with bus_load_fence
     * after observing the flags byte. */
    ucs_memory_bus_store_fence();
    elem->flags = owner_bit | UCT_OBMM_FIFO_ELEM_FLAG_BCOPY;

    if (ucs_unlikely(iface->stats_enable)) {
        iface->baseline.tx_msgs++;
        iface->baseline.tx_bytes += length;
        iface->baseline.tx_bcopy_msgs++;
    }
    UCT_TL_EP_STAT_OP(&ep->super, AM, BCOPY, length);
    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, id,
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
    if (ucs_unlikely(iface->stats_enable)) {
        iface->baseline.pending_queued++;
    }

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

    /* Refresh cached tail so the request callback's am_short/am_bcopy sees
     * the freshest peer state and is not falsely starved. */
    if (!uct_obmm_ep_has_tx_resource(ep)) {
        if (ucs_unlikely(iface->stats_enable)) {
            iface->baseline.pending_resched_nores++;
        }
        return UCS_ARBITER_CB_RESULT_RESCHED_GROUP;
    }

    req    = ucs_container_of(elem, uct_pending_req_t, priv);
    status = req->func(req);

    if (status == UCS_OK) {
        if (ucs_unlikely(iface->stats_enable)) {
            iface->baseline.pending_completed++;
        }
        ++(*count);
        return UCS_ARBITER_CB_RESULT_REMOVE_ELEM;
    } else if (status == UCS_INPROGRESS) {
        if (ucs_unlikely(iface->stats_enable)) {
            iface->baseline.pending_inprogress++;
        }
        ++(*count);
        return UCS_ARBITER_CB_RESULT_NEXT_GROUP;
    }

    /* NO_RESOURCE (or any other transient): keep the request and try
     * again the next time iface_progress runs. */
    if (ucs_unlikely(iface->stats_enable)) {
        iface->baseline.pending_resched_retry++;
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
