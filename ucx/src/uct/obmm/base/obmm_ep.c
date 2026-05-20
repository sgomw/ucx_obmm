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
#include <ucs/sys/math.h>

#include <string.h>

static volatile uint32_t uct_obmm_lock_token_starttime_warned = 0;

static UCS_F_ALWAYS_INLINE uint64_t uct_obmm_ep_mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31;
    return x;
}

static UCS_F_ALWAYS_INLINE uint64_t
uct_obmm_ep_make_lock_token(uct_obmm_iface_t *iface, const uct_obmm_ep_t *ep)
{
    uint64_t token;
    uint64_t self_id;
    unsigned long self_starttime;

    self_starttime = ucs_sys_get_proc_create_time(getpid());
    if ((self_starttime == 0ul) &&
        (ucs_atomic_cswap32(&uct_obmm_lock_token_starttime_warned, 0, 1) == 0)) {
        ucs_warn("obmm: cannot read self process start time while building "
                 "lock token; token uniqueness falls back to pid/slot/ep identity");
    }
    self_id        = ((uint64_t)(uint32_t)getpid() << 32) ^
                     (uint64_t)self_starttime;

    token = UINT64_C(0x9e3779b97f4a7c15);
    token = uct_obmm_ep_mix64(token ^ iface->region->info.exporter_dcna);
    token = uct_obmm_ep_mix64(token ^ iface->region->info.exporter_deid.hi);
    token = uct_obmm_ep_mix64(token ^ iface->region->info.exporter_deid.lo);
    token = uct_obmm_ep_mix64(token ^
                              (((uint64_t)iface->slot_index << 32) |
                               iface->generation));
    token = uct_obmm_ep_mix64(token ^ self_id);
    token = uct_obmm_ep_mix64(token ^
                              (((uint64_t)ep->peer_slot_index << 32) |
                               ep->peer_pid));
    token = uct_obmm_ep_mix64(token ^ (uint64_t)(uintptr_t)ep);
    return (token == 0) ? 1 : token;
}

#define UCT_OBMM_TRACE_RESERVE_LOG_MIN UINT64_C(1024)

static UCS_F_ALWAYS_INLINE int uct_obmm_trace_should_log(uint64_t count)
{
    return (count != 0) && ucs_is_pow2_or_zero(count);
}

static UCS_F_ALWAYS_INLINE int
uct_obmm_trace_should_log_persistent(uint64_t count, uint64_t min_count)
{
    return (count >= min_count) && uct_obmm_trace_should_log(count);
}

static UCS_F_ALWAYS_INLINE const char*
uct_obmm_ep_trace_reason(uint64_t head, uint64_t cached_tail, uint64_t tail,
                         uint64_t lock, unsigned fifo_size)
{
    if (lock != 0) {
        return "lock-busy";
    }

    if (((head - cached_tail) >= fifo_size) && ((head - tail) >= fifo_size)) {
        return "fifo-full";
    }

    return "state-race";
}

static UCS_F_ALWAYS_INLINE void
uct_obmm_ep_trace_state(uct_obmm_ep_t *ep, const char *stage, uint64_t count,
                        const char *detail, const uct_pending_req_t *req)
{
    uint64_t head;
    uint64_t cached_tail;
    uint64_t tail;
    uint64_t lock;

    ucs_memory_bus_load_fence();
    head = ep->peer_ctl->head;
    cached_tail = ep->cached_tail;
    tail = ep->peer_ctl->tail;
    lock = ep->peer_ctl->lock;

    ucs_warn("obmm: %s x%llu ep=%p req=%p peer(pid=%u slot=%u gen=%u) "
             "reason=%s detail=%s lock=0x%llx token=0x%llx head=%llu "
             "cached_tail=%llu tail=%llu group_empty=%d",
             stage, (unsigned long long)count, ep, req, ep->peer_pid,
             ep->peer_slot_index, ep->expected_generation,
             uct_obmm_ep_trace_reason(head, cached_tail, tail, lock,
                                      ep->fifo_size),
             detail,
             (unsigned long long)lock, (unsigned long long)ep->lock_token,
             (unsigned long long)head, (unsigned long long)cached_tail,
             (unsigned long long)tail,
             ucs_arbiter_group_is_empty(&ep->arb_group));
}

static UCS_F_ALWAYS_INLINE void
uct_obmm_ep_trace_recovered(uct_obmm_ep_t *ep, const char *stage,
                            uint64_t count, const uct_pending_req_t *req)
{
    if (count == 0) {
        return;
    }

    uct_obmm_ep_trace_state(ep, stage, count, "recovered", req);
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

static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_reserve_slot(uct_obmm_ep_t *ep, uint64_t *head_p);

static UCS_F_ALWAYS_INLINE int
uct_obmm_ep_has_tx_resource(uct_obmm_ep_t *ep);


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
    self->lock_token          = uct_obmm_ep_make_lock_token(iface, self);
    self->trace_reserve_no_resource_count = 0;
    self->trace_pending_queue_count       = 0;
    self->trace_pending_resched_count     = 0;
    self->trace_send_with_pending_count   = 0;

    ucs_warn("obmm: ep token map ep=%p token=0x%llx local(pid=%d slot=%u "
             "gen=%u) peer(pid=%u slot=%u dcna=0x%llx deid=0x%llx:0x%llx)",
             self, (unsigned long long)self->lock_token, getpid(),
             iface->slot_index, iface->generation, self->peer_pid,
             self->peer_slot_index, (unsigned long long)self->peer_dcna,
             (unsigned long long)self->peer_deid_hi,
             (unsigned long long)self->peer_deid_lo);
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
    uct_obmm_fifo_element_t *elem;
    uint64_t                 head;
    uint8_t                  owner_bit;

    UCT_CHECK_AM_ID(id);
    UCT_CHECK_LENGTH(payload_total, 0,
                     ep->fifo_elem_size - sizeof(uct_obmm_fifo_element_t),
                     "am_short");

    if (ucs_unlikely(!ucs_arbiter_group_is_empty(&ep->arb_group))) {
        ++ep->trace_send_with_pending_count;
        if (uct_obmm_trace_should_log(ep->trace_send_with_pending_count)) {
            uct_obmm_ep_trace_state(ep, "send while pending",
                                    ep->trace_send_with_pending_count,
                                    "am_short entry", NULL);
        }
    }

    if (uct_obmm_ep_reserve_slot(ep, &head) != UCS_OK) {
        return UCS_ERR_NO_RESOURCE;
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
    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, id,
                       &header, payload_total, "TX: AM_SHORT");
    return UCS_OK;
}


/* Reserve one slot in the peer's FIFO, returning the head index that was
 * claimed. Multi-producer reservation is serialized with a token lock because
 * on target NC/aarch64 mappings, CAS updates memory but its return value is
 * not a reliable ownership result. */
static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_reserve_slot(uct_obmm_ep_t *ep, uint64_t *head_p)
{
    uint64_t head;

    if (!uct_obmm_ep_try_lock_head(ep)) {
        ++ep->trace_reserve_no_resource_count;
        if (uct_obmm_trace_should_log_persistent(
                ep->trace_reserve_no_resource_count,
                UCT_OBMM_TRACE_RESERVE_LOG_MIN)) {
            uct_obmm_ep_trace_state(ep, "reserve stalled",
                                    ep->trace_reserve_no_resource_count,
                                    "head lock busy", NULL);
        }
        UCS_STATS_UPDATE_COUNTER(ep->super.stats, UCT_EP_STAT_NO_RES, 1);
        return UCS_ERR_NO_RESOURCE;
    }

    ucs_memory_bus_load_fence();
    head = ep->peer_ctl->head;

    if ((head - ep->cached_tail) >= ep->fifo_size) {
        ucs_memory_bus_load_fence();
        ep->cached_tail = ep->peer_ctl->tail;
        if ((head - ep->cached_tail) >= ep->fifo_size) {
            ++ep->trace_reserve_no_resource_count;
            if (uct_obmm_trace_should_log_persistent(
                    ep->trace_reserve_no_resource_count,
                    UCT_OBMM_TRACE_RESERVE_LOG_MIN)) {
                uct_obmm_ep_trace_state(ep, "reserve stalled",
                                        ep->trace_reserve_no_resource_count,
                                        "peer fifo full", NULL);
            }
            UCS_STATS_UPDATE_COUNTER(ep->super.stats, UCT_EP_STAT_NO_RES, 1);
            uct_obmm_ep_unlock_head(ep);
            return UCS_ERR_NO_RESOURCE;
        }
    }

    uct_obmm_bus_full_fence();
    ep->peer_ctl->head = head + 1;
    uct_obmm_ep_unlock_head(ep);

    if (uct_obmm_trace_should_log_persistent(ep->trace_reserve_no_resource_count,
                                             UCT_OBMM_TRACE_RESERVE_LOG_MIN)) {
        uct_obmm_ep_trace_recovered(ep, "reserve recovered",
                                    ep->trace_reserve_no_resource_count, NULL);
    }
    ep->trace_reserve_no_resource_count = 0;

    *head_p = head;
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
    uint64_t                 head;
    size_t                   length;
    uint8_t                  owner_bit;
    ucs_status_t             status;

    /* flags (UCT_SEND_FLAG_PEER_CHECK etc.) are ignored: this transport
     * does not advertise EP_CHECK / keepalive in v1. */
    (void)flags;

    UCT_CHECK_AM_ID(id);

    if (ucs_unlikely(!ucs_arbiter_group_is_empty(&ep->arb_group))) {
        ++ep->trace_send_with_pending_count;
        if (uct_obmm_trace_should_log(ep->trace_send_with_pending_count)) {
            uct_obmm_ep_trace_state(ep, "send while pending",
                                    ep->trace_send_with_pending_count,
                                    "am_bcopy entry", NULL);
        }
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
    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, id,
                       desc, length, "TX: AM_BCOPY");
    return (ssize_t)length;
}


/* Conservative best-effort check for whether a pending request is worth
 * retrying in this progress round. Returns false if the peer FIFO is full or
 * if the peer head lock is observed busy. Because another producer can race
 * us after this check, callers must still tolerate reserve_slot() returning
 * UCS_ERR_NO_RESOURCE and simply reschedule the request. */
static UCS_F_ALWAYS_INLINE int
uct_obmm_ep_has_tx_resource(uct_obmm_ep_t *ep)
{
    uint64_t head;

    ucs_memory_bus_load_fence();
    if (ep->peer_ctl->lock != 0) {
        return 0;
    }

    ucs_memory_bus_load_fence();
    head = ep->peer_ctl->head;
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

    /* obmm NO_RESOURCE can mean either FIFO-full or transient head-lock
     * contention. Queue unconditionally rather than returning BUSY for an
     * immediate retry, otherwise multi-producer contention can devolve into a
     * retry storm instead of making forward progress through the iface arbiter. */

    ++ep->trace_pending_queue_count;
    if (uct_obmm_trace_should_log(ep->trace_pending_queue_count)) {
        uct_obmm_ep_trace_state(ep, "pending queued",
                                ep->trace_pending_queue_count,
                                "pending_add", n);
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
        ++ep->trace_pending_resched_count;
        if (uct_obmm_trace_should_log(ep->trace_pending_resched_count)) {
            uct_obmm_ep_trace_state(ep, "pending stalled",
                                    ep->trace_pending_resched_count,
                                    "has_tx_resource=false", NULL);
        }
        return UCS_ARBITER_CB_RESULT_RESCHED_GROUP;
    }

    req    = ucs_container_of(elem, uct_pending_req_t, priv);
    status = req->func(req);

    if (status == UCS_OK) {
        uct_obmm_ep_trace_recovered(ep, "pending recovered",
                                    ep->trace_pending_resched_count, req);
        ep->trace_pending_resched_count = 0;
        ++(*count);
        return UCS_ARBITER_CB_RESULT_REMOVE_ELEM;
    } else if (status == UCS_INPROGRESS) {
        uct_obmm_ep_trace_recovered(ep, "pending progressed",
                                    ep->trace_pending_resched_count, req);
        ep->trace_pending_resched_count = 0;
        ++(*count);
        return UCS_ARBITER_CB_RESULT_NEXT_GROUP;
    }

    /* NO_RESOURCE (or any other transient): keep the request and try
     * again the next time iface_progress runs. */
    ++ep->trace_pending_resched_count;
    if (uct_obmm_trace_should_log(ep->trace_pending_resched_count)) {
        uct_obmm_ep_trace_state(ep, "pending callback stalled",
                                ep->trace_pending_resched_count,
                                ucs_status_string(status), req);
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
