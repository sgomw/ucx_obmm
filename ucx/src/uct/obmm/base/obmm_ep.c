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

#include <uct/base/uct_iov.inl>
#include <uct/base/uct_log.h>
#include <uct/base/uct_iface.h>
#include <ucs/arch/atomic.h>
#include <ucs/arch/cpu.h>
#include <ucs/debug/log.h>
#include <ucs/debug/memtrack_int.h>
#include <ucs/sys/math.h>
#include <ucs/sys/ptr_arith.h>

#include <inttypes.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_reserve_slot(uct_obmm_ep_t *ep, uint64_t *head_p);
static void uct_obmm_cc_complete_flush(uct_obmm_iface_t *iface,
                                       uct_obmm_ep_t *ep);

typedef struct uct_obmm_cc_pending_ack {
    struct uct_obmm_cc_pending_ack *next;
    uct_obmm_cc_data_ready_t        ready;
} uct_obmm_cc_pending_ack_t;

static UCS_CLASS_INIT_FUNC(uct_obmm_ep_t, const uct_ep_params_t *params)
{
    uct_obmm_iface_t             *iface = ucs_derived_of(params->iface,
                                                         uct_obmm_iface_t);
    uct_obmm_md_t                *md    = ucs_derived_of(iface->super.md,
                                                         uct_obmm_md_t);
    const uct_obmm_device_addr_t *daddr;
    const uct_obmm_iface_addr_t  *iaddr;
    uct_obmm_eid_t                eid;
    uct_obmm_region_t            *region;
    uct_obmm_region_t            *cc_region = NULL;
    uct_obmm_pool_t               peer_pool;
    void                         *peer_slot;
    ucs_status_t                  status;
    uint32_t                      expected_wire_format;

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);

    ucs_arbiter_group_init(&self->arb_group);
    self->cc_outstanding = 0;
    self->cc_flush_comp  = NULL;
    self->peer_cc_region = NULL;
    self->peer_cc_slot_base = NULL;
    self->cached_cc_tail = 0;

    daddr = (const uct_obmm_device_addr_t*)params->dev_addr;
    iaddr = (const uct_obmm_iface_addr_t*)params->iface_addr;
    if ((params->field_mask & UCT_EP_PARAM_FIELD_IFACE_ADDR_LENGTH) &&
        (params->iface_addr_length < sizeof(*iaddr))) {
        ucs_error("obmm: iface address too short: peer=%zu local=%zu",
                  params->iface_addr_length, sizeof(*iaddr));
        return UCS_ERR_UNREACHABLE;
    }

    /* Reject incompatible geometry. UCX wireup should already have filtered
     * this out via is_reachable_v2, but double-check. */
    expected_wire_format = iface->cc.enabled ? UCT_OBMM_WIRE_FORMAT_CCZCOPY :
                           UCT_OBMM_WIRE_FORMAT_INLINE32;

    if ((iaddr->slot_count != UCT_OBMM_POOL_SLOT_COUNT) ||
        (iaddr->short_lane_count != UCT_OBMM_SHORT_LANE_COUNT) ||
        (iaddr->wire_format != expected_wire_format) ||
        (iaddr->fifo_size != iface->fifo_size) ||
        (iaddr->fifo_elem_size != iface->fifo_elem_size) ||
        (iaddr->bcopy_seg_size != iface->bcopy_seg_size) ||
        (iaddr->cc_chunk_count !=
         (iface->cc.enabled ? iface->cc.chunk_count : 0)) ||
        (iaddr->cc_chunk_size !=
         (iface->cc.enabled ? iface->cc.chunk_size : 0)) ||
        (iaddr->cc_min_zcopy !=
         (iface->cc.enabled ? iface->cc.min_zcopy : 0)) ||
        (iaddr->cc_own_granule !=
         (iface->cc.enabled ? iface->cc.own_granule : 0))) {
        ucs_error("obmm: peer geometry (slots=%u lanes=%u wire=%u fifo=%u "
                  "elem=%u seg=%u cc_count=%u cc_size=%u cc_min=%u "
                  "cc_gran=%u) differs from local (slots=%u lanes=%u "
                  "wire=%u fifo=%u elem=%u seg=%u cc_count=%u cc_size=%zu "
                  "cc_min=%zu cc_gran=%zu); ep_create rejected",
                  iaddr->slot_count, iaddr->short_lane_count,
                  iaddr->wire_format, iaddr->fifo_size, iaddr->fifo_elem_size,
                  iaddr->bcopy_seg_size, iaddr->cc_chunk_count,
                  iaddr->cc_chunk_size, iaddr->cc_min_zcopy,
                  iaddr->cc_own_granule,
                  UCT_OBMM_POOL_SLOT_COUNT, UCT_OBMM_SHORT_LANE_COUNT,
                  expected_wire_format,
                  iface->fifo_size, iface->fifo_elem_size,
                  iface->bcopy_seg_size,
                  iface->cc.enabled ? iface->cc.chunk_count : 0,
                  iface->cc.enabled ? iface->cc.chunk_size : 0,
                  iface->cc.enabled ? iface->cc.min_zcopy : 0,
                  iface->cc.enabled ? iface->cc.own_granule : 0);
        return UCS_ERR_UNREACHABLE;
    }

    /* Find the local region (export-for-self, import-for-remote) that maps
     * the peer's exporter coordinates. */
    {
        eid.hi = daddr->exporter_deid_hi;
        eid.lo = daddr->exporter_deid_lo;

        region = uct_obmm_md_find_region(md, UCT_OBMM_REGION_KIND_NC,
                                         daddr->exporter_dcna, &eid);
    }
    if (region == NULL) {
        ucs_error("obmm: ep_create cannot find region for peer "
                  "dcna=0x%lx deid=0x%lx:0x%lx",
                  (unsigned long)daddr->exporter_dcna,
                  (unsigned long)daddr->exporter_deid_hi,
                  (unsigned long)daddr->exporter_deid_lo);
        return UCS_ERR_UNREACHABLE;
    }

    if (iface->cc.enabled) {
        cc_region = uct_obmm_md_find_region(md, UCT_OBMM_REGION_KIND_CC,
                                            daddr->exporter_dcna, &eid);
        if (cc_region == NULL) {
            ucs_error("obmm: ep_create cannot find CC region for peer "
                      "dcna=0x%lx deid=0x%lx:0x%lx",
                      (unsigned long)daddr->exporter_dcna,
                      (unsigned long)daddr->exporter_deid_hi,
                      (unsigned long)daddr->exporter_deid_lo);
            return UCS_ERR_UNREACHABLE;
        }
    }

    status = uct_obmm_pool_open(region->base, region->length, &peer_pool);
    if (status != UCS_OK) {
        ucs_error("obmm: failed to open peer pool: %s",
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
    if (iface->cc.enabled) {
        self->peer_cc_region    = cc_region;
        self->peer_cc_slot_base = UCS_PTR_BYTE_OFFSET(
                                  cc_region->base,
                                  (size_t)iaddr->slot_index *
                                  iface->cc.slot_stride);
        self->cached_cc_tail    = self->peer_ctl->cc_tail;
    }
    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_obmm_ep_t)
{
    if (self->cc_outstanding > 0) {
        ucs_warn("obmm: destroying ep with %u outstanding CC zcopy chunks",
                 self->cc_outstanding);
    }
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
    const uct_obmm_iface_t       *iface;
    const uct_obmm_device_addr_t *daddr;
    const uct_obmm_iface_addr_t  *iaddr;
    uint32_t                      expected_wire_format;

    if (!uct_base_ep_is_connected(tl_ep, params)) {
        return 0;
    }

    iface = ucs_derived_of(tl_ep->iface, uct_obmm_iface_t);
    expected_wire_format = iface->cc.enabled ? UCT_OBMM_WIRE_FORMAT_CCZCOPY :
                           UCT_OBMM_WIRE_FORMAT_INLINE32;

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
           (iaddr->wire_format == expected_wire_format) &&
           (iaddr->fifo_size == ep->fifo_size) &&
           (iaddr->fifo_elem_size == ep->fifo_elem_size) &&
           (iaddr->bcopy_seg_size == ep->bcopy_seg_size) &&
           (iaddr->cc_chunk_count ==
            (iface->cc.enabled ? iface->cc.chunk_count : 0)) &&
           (iaddr->cc_chunk_size ==
            (iface->cc.enabled ? iface->cc.chunk_size : 0)) &&
           (iaddr->cc_min_zcopy ==
            (iface->cc.enabled ? iface->cc.min_zcopy : 0)) &&
           (iaddr->cc_own_granule ==
            (iface->cc.enabled ? iface->cc.own_granule : 0)) &&
           (iaddr->slot_index == ep->peer_slot_index) &&
           (iaddr->generation == ep->expected_generation);
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

static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_reserve_fifo_direct(uct_obmm_fifo_ctl_t *ctl, unsigned fifo_size,
                             uint64_t *head_p)
{
    uint64_t head, tail;

    tail = ctl->tail;
    for (;;) {
        head = ctl->head;

        if ((head - tail) >= fifo_size) {
            ucs_memory_bus_load_fence();
            tail = ctl->tail;
            if ((head - tail) >= fifo_size) {
                return UCS_ERR_NO_RESOURCE;
            }
        }

        if (uct_obmm_atomic_bool_cswap64(&ctl->head, head, head + 1)) {
            *head_p = head;
            return UCS_OK;
        }
    }
}


static ucs_status_t
uct_obmm_write_cc_ack(uct_obmm_iface_t *iface,
                      const uct_obmm_cc_data_ready_t *ready)
{
    uct_obmm_md_t           *md = ucs_derived_of(iface->super.md,
                                                 uct_obmm_md_t);
    uct_obmm_eid_t           eid;
    uct_obmm_region_t       *region;
    uct_obmm_pool_t          pool;
    void                    *slot;
    uct_obmm_fifo_ctl_t     *ctl;
    void                    *elems;
    uct_obmm_fifo_element_t *elem;
    uct_obmm_cc_ack_t        ack;
    uint64_t                 head;
    uint8_t                  owner_bit;
    ucs_status_t             status;

    eid.hi = ready->sender_deid_hi;
    eid.lo = ready->sender_deid_lo;

    region = uct_obmm_md_find_region(md, UCT_OBMM_REGION_KIND_NC,
                                     ready->sender_dcna, &eid);
    if (region == NULL) {
        ucs_error("obmm: cannot ACK CC chunk; no NC region for sender "
                  "dcna=0x%" PRIx64 " deid=0x%" PRIx64 ":0x%" PRIx64,
                  ready->sender_dcna, ready->sender_deid_hi,
                  ready->sender_deid_lo);
        return UCS_ERR_UNREACHABLE;
    }

    status = uct_obmm_pool_open(region->base, region->length, &pool);
    if (status != UCS_OK) {
        return status;
    }

    if (ready->sender_slot_index >= pool.slot_count) {
        return UCS_ERR_INVALID_PARAM;
    }

    if (pool.slot_size != uct_obmm_slot_stride(iface->fifo_size,
                                               iface->fifo_elem_size,
                                               iface->bcopy_seg_size)) {
        return UCS_ERR_INVALID_PARAM;
    }

    slot  = uct_obmm_pool_slot_ptr(&pool, ready->sender_slot_index);
    ctl   = uct_obmm_slot_ctl(slot);
    elems = uct_obmm_slot_elems(slot);

    status = uct_obmm_reserve_fifo_direct(ctl, iface->fifo_size, &head);
    if (status != UCS_OK) {
        return status;
    }

    ack.seq               = ready->seq;
    ack.chunk_id          = ready->chunk_id;
    ack.sender_generation = ready->sender_generation;

    elem             = uct_obmm_slot_elem(elems, head, iface->fifo_mask,
                                          iface->fifo_elem_size);
    elem->am_id      = 0;
    elem->length     = sizeof(ack);
    elem->generation = ready->sender_generation;
    memcpy((char*)elem + ucs_offsetof(uct_obmm_fifo_element_t, header),
           &ack, sizeof(ack));

    owner_bit = (head & iface->fifo_size) ? 0u :
                                        UCT_OBMM_FIFO_ELEM_FLAG_OWNER;
    ucs_memory_bus_store_fence();
    elem->flags = owner_bit | UCT_OBMM_FIFO_ELEM_FLAG_CC_ACK;
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
     * to bcopy_seg_size. The assert catches buggy direct UCT users in
     * debug builds; production safety relies on the iface cap contract. */
    length = pack_cb(desc, arg);
    ucs_assertv(length <= ep->bcopy_seg_size,
                "obmm: pack_cb returned %zu > bcopy_seg_size=%u",
                length, ep->bcopy_seg_size);

    elem->am_id      = id;
    elem->length     = (uint32_t)length;
    elem->generation = ep->expected_generation;
    elem->header     = 0; /* unused for bcopy */

    owner_bit = (head & ep->fifo_size) ? 0u :
                                        UCT_OBMM_FIFO_ELEM_FLAG_OWNER;

    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, id,
                       desc, length, "TX: AM_BCOPY");

    /* Release barrier: orders the desc[N] payload writes AND elem header
     * writes BEFORE the flags publish. Receiver pairs with bus_load_fence
     * after observing the flags byte. */
    ucs_memory_bus_store_fence();
    elem->flags = owner_bit | UCT_OBMM_FIFO_ELEM_FLAG_BCOPY;

    UCT_TL_EP_STAT_OP(&ep->super, AM, BCOPY, length);
    return (ssize_t)length;
}


static ucs_status_t
uct_obmm_ep_reserve_cc_chunk(uct_obmm_ep_t *ep, uct_obmm_iface_t *iface,
                             uint64_t *cc_seq_p, uint32_t *chunk_id_p)
{
    uint64_t head;

    if (ep->peer_cc_region == NULL) {
        return UCS_ERR_UNSUPPORTED;
    }

    for (;;) {
        head = ep->peer_ctl->cc_head;

        if ((head - ep->cached_cc_tail) >= iface->cc.chunk_count) {
            ucs_memory_bus_load_fence();
            ep->cached_cc_tail = ep->peer_ctl->cc_tail;
            if ((head - ep->cached_cc_tail) >= iface->cc.chunk_count) {
                UCS_STATS_UPDATE_COUNTER(ep->super.stats,
                                         UCT_EP_STAT_NO_RES, 1);
                return UCS_ERR_NO_RESOURCE;
            }
        }

        if (uct_obmm_atomic_bool_cswap64(&ep->peer_ctl->cc_head, head,
                                         head + 1)) {
            *cc_seq_p   = head;
            *chunk_id_p = (uint32_t)(head % iface->cc.chunk_count);
            return UCS_OK;
        }
    }
}


static void uct_obmm_cc_copy_iov(void *dst, const uct_iov_t *iov,
                                 size_t iovcnt)
{
    char     *pos = dst;
    size_t    i, j;
    uintptr_t stride;

    for (i = 0; i < iovcnt; ++i) {
        if ((iov[i].length == 0) || (iov[i].count == 0)) {
            continue;
        }

        stride = (iov[i].count > 1) ? iov[i].stride : iov[i].length;
        for (j = 0; j < iov[i].count; ++j) {
            memcpy(pos, UCS_PTR_BYTE_OFFSET(iov[i].buffer, j * stride),
                   iov[i].length);
            pos += iov[i].length;
        }
    }
}


static size_t
uct_obmm_cc_ownership_length(const uct_obmm_iface_t *iface, size_t length)
{
    size_t own_length = ucs_align_up(length, iface->cc.own_granule);

    return ucs_max(own_length, iface->cc.own_granule);
}


static unsigned uct_obmm_cc_diag_bucket_index(size_t length)
{
    if (length <= ((512u + 64u) * 1024u)) {
        return 0;
    } else if (length <= ((1024u + 128u) * 1024u)) {
        return 1;
    } else if (length <= ((2u * 1024u + 256u) * 1024u)) {
        return 2;
    } else if (length <= ((4u * 1024u + 512u) * 1024u)) {
        return 3;
    }

    return 4;
}


static const char *uct_obmm_cc_diag_bucket_name(unsigned bucket)
{
    static const char *names[UCT_OBMM_CC_DIAG_BUCKETS] = {
        "le512K",
        "le1M",
        "le2M",
        "le4M",
        "gt4M"
    };

    return names[bucket];
}


static uint64_t uct_obmm_cc_diag_nsec(ucs_time_t interval)
{
    double nsec = ucs_time_to_nsec(interval);

    return (nsec > 0.0) ? (uint64_t)(nsec + 0.5) : 0;
}


static void uct_obmm_cc_diag_add_time(uint64_t *sum, ucs_time_t interval)
{
    *sum += uct_obmm_cc_diag_nsec(interval);
}


static double uct_obmm_cc_diag_avg_us(uint64_t nsec, uint64_t count)
{
    return (count == 0) ? 0.0 : ((double)nsec / (double)count / 1000.0);
}


static uint64_t uct_obmm_cc_diag_avg_len(uint64_t bytes, uint64_t count)
{
    return (count == 0) ? 0 : (bytes / count);
}


static void uct_obmm_cc_diag_print(uct_obmm_iface_t *iface)
{
    const uct_obmm_cc_diag_bucket_t *diag;
    unsigned                         i;

    if (!iface->cc.diag_enabled) {
        return;
    }

    for (i = 0; i < UCT_OBMM_CC_DIAG_BUCKETS; ++i) {
        diag = &iface->cc.diag[i];
        if ((diag->tx_calls == 0) && (diag->rx_calls == 0) &&
            (diag->tx_short_fallback == 0) &&
            (diag->tx_cc_nores == 0) && (diag->tx_ready_nores == 0) &&
            (diag->rx_ack_nores == 0)) {
            continue;
        }

        fprintf(stderr,
                "obmm_cc_diag: rank=%d pid=%ld slot=%u bucket=%s "
                "tx_calls=%" PRIu64 " tx_avg_len=%" PRIu64 " "
                "tx_short_fallback=%" PRIu64 " tx_short_avg_len=%" PRIu64 " "
                "tx_short_nores=%" PRIu64 " "
                "tx_nores_cc=%" PRIu64 " tx_ready_nores=%" PRIu64 " "
                "tx_ready_deferred=%" PRIu64 " tx_acks=%" PRIu64 " "
                "tx_us reserve=%.3f write=%.3f copy=%.3f none=%.3f "
                "ready=%.3f ackwait=%.3f ackmax=%.3f rx_calls=%" PRIu64 " "
                "rx_avg_len=%" PRIu64 " rx_gap_max=%" PRIu64 " "
                "rx_gap_count=%" PRIu64 " rx_ack_nores=%" PRIu64 " "
                "rx_ack_deferred=%" PRIu64 " rx_us read=%.3f cb=%.3f "
                "none=%.3f ack=%.3f\n",
                iface->cc.diag_rank, (long)getpid(), iface->slot_index,
                uct_obmm_cc_diag_bucket_name(i), diag->tx_calls,
                uct_obmm_cc_diag_avg_len(diag->tx_bytes, diag->tx_calls),
                diag->tx_short_fallback,
                uct_obmm_cc_diag_avg_len(diag->tx_short_fallback_bytes,
                                         diag->tx_short_fallback),
                diag->tx_short_fallback_nores,
                diag->tx_cc_nores, diag->tx_ready_nores,
                diag->tx_ready_deferred, diag->tx_ack_count,
                uct_obmm_cc_diag_avg_us(diag->tx_reserve_nsec,
                                         diag->tx_calls),
                uct_obmm_cc_diag_avg_us(diag->tx_write_own_nsec,
                                         diag->tx_calls),
                uct_obmm_cc_diag_avg_us(diag->tx_copy_nsec,
                                         diag->tx_calls),
                uct_obmm_cc_diag_avg_us(diag->tx_none_own_nsec,
                                         diag->tx_calls),
                uct_obmm_cc_diag_avg_us(diag->tx_ready_nsec,
                                         diag->tx_calls),
                uct_obmm_cc_diag_avg_us(diag->tx_ack_wait_nsec,
                                         diag->tx_ack_count),
                diag->tx_ack_wait_max_nsec / 1000.0, diag->rx_calls,
                uct_obmm_cc_diag_avg_len(diag->rx_bytes, diag->rx_calls),
                diag->rx_gap_max, diag->rx_gap_count, diag->rx_ack_nores,
                diag->rx_ack_deferred,
                uct_obmm_cc_diag_avg_us(diag->rx_read_own_nsec,
                                         diag->rx_calls),
                uct_obmm_cc_diag_avg_us(diag->rx_cb_nsec,
                                         diag->rx_calls),
                uct_obmm_cc_diag_avg_us(diag->rx_none_own_nsec,
                                         diag->rx_calls),
                uct_obmm_cc_diag_avg_us(diag->rx_ack_nsec,
                                         diag->rx_calls));
    }
}


static ucs_status_t
uct_obmm_ep_send_cc_data_ready(uct_obmm_ep_t *ep,
                               const uct_obmm_cc_data_ready_t *ready,
                               uint8_t am_id)
{
    uct_obmm_fifo_element_t *elem;
    uint64_t                 head;
    uint8_t                  owner_bit;
    ucs_status_t             status;

    status = uct_obmm_ep_reserve_slot(ep, &head);
    if (status != UCS_OK) {
        return status;
    }

    elem             = uct_obmm_slot_elem(ep->peer_elems, head, ep->fifo_mask,
                                          ep->fifo_elem_size);
    elem->am_id      = am_id;
    elem->length     = sizeof(*ready);
    elem->generation = ep->expected_generation;
    memcpy((char*)elem + ucs_offsetof(uct_obmm_fifo_element_t, header),
           ready, sizeof(*ready));

    owner_bit = (head & ep->fifo_size) ? 0u :
                                        UCT_OBMM_FIFO_ELEM_FLAG_OWNER;
    ucs_memory_bus_store_fence();
    elem->flags = owner_bit | UCT_OBMM_FIFO_ELEM_FLAG_CC_DATA_READY;
    return UCS_OK;
}


static void
uct_obmm_iface_tx_list_remove(uct_obmm_iface_t *iface,
                              uct_obmm_cc_tx_slot_t *tx_slot,
                              uct_obmm_cc_tx_slot_t *prev)
{
    if (prev == NULL) {
        iface->cc.tx_slots = tx_slot->next;
    } else {
        prev->next = tx_slot->next;
    }
}


static ucs_status_t
uct_obmm_cc_try_send_ready(uct_obmm_cc_tx_slot_t *tx_slot)
{
    uct_obmm_iface_t            *iface = ucs_derived_of(
                                         tx_slot->ep->super.super.iface,
                                         uct_obmm_iface_t);
    uct_obmm_cc_diag_bucket_t   *diag  = NULL;
    ucs_time_t                   start_time = 0;
    ucs_status_t                 status;

    if (tx_slot->ready_sent) {
        return UCS_OK;
    }

    if (iface->cc.diag_enabled) {
        diag       = &iface->cc.diag[tx_slot->diag_bucket];
        start_time = ucs_get_time();
    }

    status = uct_obmm_ep_send_cc_data_ready(tx_slot->ep, &tx_slot->ready,
                                            tx_slot->am_id);
    if (diag != NULL) {
        uct_obmm_cc_diag_add_time(&diag->tx_ready_nsec,
                                  ucs_get_time() - start_time);
    }

    if (status == UCS_OK) {
        if ((diag != NULL) && (tx_slot->ready_attempts > 0)) {
            diag->tx_ready_deferred++;
        }
        tx_slot->ready_sent = 1;
    } else if (status == UCS_ERR_NO_RESOURCE) {
        if (diag != NULL) {
            diag->tx_ready_nores++;
        }
        if (tx_slot->ready_attempts < 255u) {
            tx_slot->ready_attempts++;
        }
    }

    return status;
}


unsigned uct_obmm_iface_progress_cc_ready(uct_obmm_iface_t *iface)
{
    uct_obmm_cc_tx_slot_t *tx_slot;
    uct_obmm_cc_tx_slot_t *prev = NULL;
    uct_obmm_cc_tx_slot_t *next;
    ucs_status_t           status;
    unsigned               count = 0;

    for (tx_slot = iface->cc.tx_slots; tx_slot != NULL; tx_slot = next) {
        next = tx_slot->next;

        if (tx_slot->ready_sent) {
            prev = tx_slot;
            continue;
        }

        status = uct_obmm_cc_try_send_ready(tx_slot);
        if (status == UCS_OK) {
            ++count;
            prev = tx_slot;
        } else if (status == UCS_ERR_NO_RESOURCE) {
            prev = tx_slot;
        } else {
            ucs_warn("obmm: dropping pending CC_DATA_READY after send "
                     "failure: %s", ucs_status_string(status));
            uct_obmm_iface_tx_list_remove(iface, tx_slot, prev);
            if (iface->cc.outstanding > 0) {
                iface->cc.outstanding--;
            }
            if ((tx_slot->ep != NULL) && (tx_slot->ep->cc_outstanding > 0)) {
                tx_slot->ep->cc_outstanding--;
            }
            uct_obmm_cc_complete_flush(iface, tx_slot->ep);
            ucs_free(tx_slot);
        }
    }

    return count;
}


static ucs_status_t
uct_obmm_ep_am_zcopy_short_fallback(uct_obmm_ep_t *ep,
                                    uct_obmm_iface_t *iface, uint8_t id,
                                    const void *header,
                                    unsigned header_length,
                                    const uct_iov_t *iov, size_t iovcnt,
                                    size_t total_length)
{
    uct_obmm_fifo_element_t   *elem;
    void                      *short_data;
    uint64_t                   head;
    uint8_t                    owner_bit;
    ucs_status_t               status;
    uct_obmm_cc_diag_bucket_t *diag = NULL;

    if (iface->cc.diag_enabled) {
        diag = &iface->cc.diag[uct_obmm_cc_diag_bucket_index(total_length)];
        diag->tx_short_fallback++;
        diag->tx_short_fallback_bytes += total_length;
    }

    status = uct_obmm_ep_reserve_slot(ep, &head);
    if (status != UCS_OK) {
        if ((diag != NULL) && (status == UCS_ERR_NO_RESOURCE)) {
            diag->tx_short_fallback_nores++;
        }
        return status;
    }

    elem       = uct_obmm_slot_elem(ep->peer_elems, head, ep->fifo_mask,
                                    ep->fifo_elem_size);
    short_data = (char*)elem + ucs_offsetof(uct_obmm_fifo_element_t, header);

    elem->am_id      = id;
    elem->length     = (uint32_t)total_length;
    elem->generation = ep->expected_generation;
    if (header_length > 0) {
        memcpy(short_data, header, header_length);
    }
    uct_obmm_cc_copy_iov(UCS_PTR_BYTE_OFFSET(short_data, header_length), iov,
                         iovcnt);

    owner_bit = (head & ep->fifo_size) ? 0u :
                                        UCT_OBMM_FIFO_ELEM_FLAG_OWNER;
    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, id, short_data,
                       total_length, "TX: AM_ZCOPY_SHORT_FALLBACK");
    ucs_memory_bus_store_fence();
    elem->flags = owner_bit;

    UCT_TL_EP_STAT_OP(&ep->super, AM, SHORT, total_length);
    return UCS_OK;
}


ucs_status_t uct_obmm_ep_am_zcopy(uct_ep_h tl_ep, uint8_t id,
                                  const void *header,
                                  unsigned header_length,
                                  const uct_iov_t *iov, size_t iovcnt,
                                  unsigned flags, uct_completion_t *comp)
{
    uct_obmm_ep_t            *ep    = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    uct_obmm_iface_t         *iface = ucs_derived_of(tl_ep->iface,
                                                     uct_obmm_iface_t);
    uct_obmm_cc_data_ready_t  ready;
    uct_obmm_cc_tx_slot_t    *tx_slot;
    void                     *chunk;
    size_t                    payload_length;
    size_t                    total_length;
    size_t                    own_length;
    uint64_t                  receiver_cc_seq;
    uint32_t                  chunk_id;
    ucs_status_t              status;
    uct_obmm_cc_diag_bucket_t *diag = NULL;
    unsigned                  diag_bucket = 0;
    ucs_time_t                diag_start = 0;
    ucs_time_t                time_start = 0;

    (void)flags;
    (void)comp;

    if (!iface->cc.enabled || (ep->peer_cc_region == NULL)) {
        return UCS_ERR_UNSUPPORTED;
    }

    UCT_CHECK_AM_ID(id);
    UCT_CHECK_IOV_SIZE(iovcnt, (size_t)iface->cc.max_iov, "am_zcopy");

    payload_length = uct_iov_total_length(iov, iovcnt);
    if (header_length > (SIZE_MAX - payload_length)) {
        return UCS_ERR_INVALID_PARAM;
    }
    total_length = header_length + payload_length;
    UCT_CHECK_LENGTH(total_length, 0, iface->cc.chunk_size, "am_zcopy");

    if (total_length > UINT32_MAX) {
        return UCS_ERR_INVALID_PARAM;
    }

    if ((total_length >= sizeof(uint64_t)) &&
        (total_length < iface->cc.min_zcopy) &&
        (total_length <= uct_obmm_fifo_max_short(ep->fifo_elem_size))) {
        return uct_obmm_ep_am_zcopy_short_fallback(ep, iface, id, header,
                                                   header_length, iov, iovcnt,
                                                   total_length);
    }

    own_length = uct_obmm_cc_ownership_length(iface, total_length);
    UCT_CHECK_LENGTH(own_length, 0, iface->cc.chunk_size,
                     "am_zcopy ownership");

    if (iface->cc.diag_enabled) {
        diag_bucket = uct_obmm_cc_diag_bucket_index(total_length);
        diag        = &iface->cc.diag[diag_bucket];
        diag->tx_calls++;
        diag->tx_bytes += total_length;
        diag_start = ucs_get_time();
    }

    tx_slot = ucs_calloc(1, sizeof(*tx_slot), "obmm_cc_tx_slot");
    if (tx_slot == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    if (diag != NULL) {
        time_start = ucs_get_time();
    }
    status = uct_obmm_ep_reserve_cc_chunk(ep, iface, &receiver_cc_seq,
                                          &chunk_id);
    if (diag != NULL) {
        uct_obmm_cc_diag_add_time(&diag->tx_reserve_nsec,
                                  ucs_get_time() - time_start);
    }
    if (status != UCS_OK) {
        if ((diag != NULL) && (status == UCS_ERR_NO_RESOURCE)) {
            diag->tx_cc_nores++;
        }
        ucs_free(tx_slot);
        return status;
    }

    chunk = UCS_PTR_BYTE_OFFSET(ep->peer_cc_slot_base,
                                (size_t)chunk_id * iface->cc.chunk_size);

    if (diag != NULL) {
        time_start = ucs_get_time();
    }
    status = uct_obmm_region_set_ownership(ep->peer_cc_region, chunk,
                                           own_length, PROT_WRITE);
    if (diag != NULL) {
        uct_obmm_cc_diag_add_time(&diag->tx_write_own_nsec,
                                  ucs_get_time() - time_start);
    }
    if (status != UCS_OK) {
        ucs_free(tx_slot);
        return status;
    }

    if (diag != NULL) {
        time_start = ucs_get_time();
    }
    if (header_length > 0) {
        memcpy(chunk, header, header_length);
    }
    uct_obmm_cc_copy_iov(UCS_PTR_BYTE_OFFSET(chunk, header_length), iov,
                         iovcnt);
    if (diag != NULL) {
        uct_obmm_cc_diag_add_time(&diag->tx_copy_nsec,
                                  ucs_get_time() - time_start);
    }

    if (diag != NULL) {
        time_start = ucs_get_time();
    }
    status = uct_obmm_region_set_ownership(ep->peer_cc_region, chunk,
                                           own_length, PROT_NONE);
    if (diag != NULL) {
        uct_obmm_cc_diag_add_time(&diag->tx_none_own_nsec,
                                  ucs_get_time() - time_start);
    }
    if (status != UCS_OK) {
        ucs_free(tx_slot);
        return status;
    }

    ready.sender_dcna        = iface->region->info.exporter_dcna;
    ready.sender_deid_hi     = iface->region->info.exporter_deid.hi;
    ready.sender_deid_lo     = iface->region->info.exporter_deid.lo;
    ready.seq                = iface->cc.next_seq++;
    ready.receiver_cc_seq    = receiver_cc_seq;
    ready.sender_slot_index  = iface->slot_index;
    ready.sender_generation  = iface->generation;
    ready.chunk_id           = chunk_id;
    ready.length             = (uint32_t)total_length;

    tx_slot->ready      = ready;
    tx_slot->ep         = ep;
    tx_slot->start_time = diag_start;
    tx_slot->diag_bucket = (uint8_t)diag_bucket;
    tx_slot->am_id      = id;
    tx_slot->ready_sent = 0;
    tx_slot->ready_attempts = 0;
    tx_slot->next       = iface->cc.tx_slots;
    iface->cc.tx_slots  = tx_slot;
    iface->cc.outstanding++;
    ep->cc_outstanding++;

    status = uct_obmm_cc_try_send_ready(tx_slot);
    if ((status != UCS_OK) && (status != UCS_ERR_NO_RESOURCE)) {
        iface->cc.tx_slots = tx_slot->next;
        if (iface->cc.outstanding > 0) {
            iface->cc.outstanding--;
        }
        if (ep->cc_outstanding > 0) {
            ep->cc_outstanding--;
        }
        ucs_free(tx_slot);
        return status;
    }

    UCT_TL_EP_STAT_OP(&ep->super, AM, ZCOPY, total_length);
    return UCS_OK;
}


static void uct_obmm_cc_complete_flush(uct_obmm_iface_t *iface,
                                       uct_obmm_ep_t *ep)
{
    if ((ep != NULL) && (ep->cc_outstanding == 0) &&
        (ep->cc_flush_comp != NULL)) {
        uct_completion_t *comp = ep->cc_flush_comp;

        ep->cc_flush_comp = NULL;
        uct_invoke_completion(comp, UCS_OK);
    }

    if ((iface->cc.outstanding == 0) && (iface->cc.flush_comp != NULL)) {
        uct_completion_t *comp = iface->cc.flush_comp;

        iface->cc.flush_comp = NULL;
        uct_invoke_completion(comp, UCS_OK);
    }
}


ucs_status_t uct_obmm_ep_flush(uct_ep_h tl_ep, unsigned flags,
                               uct_completion_t *comp)
{
    uct_obmm_ep_t *ep = ucs_derived_of(tl_ep, uct_obmm_ep_t);

    (void)flags;

    if (ep->cc_outstanding == 0) {
        UCT_TL_EP_STAT_FLUSH(&ep->super);
        return UCS_OK;
    }

    if (comp != NULL) {
        if (ep->cc_flush_comp != NULL) {
            return UCS_ERR_NO_RESOURCE;
        }
        ep->cc_flush_comp = comp;
    }

    UCT_TL_EP_STAT_FLUSH_WAIT(&ep->super);
    return UCS_INPROGRESS;
}


void uct_obmm_iface_handle_cc_ack(uct_obmm_iface_t *iface,
                                  const uct_obmm_cc_ack_t *ack)
{
    uct_obmm_cc_tx_slot_t *tx_slot;
    uct_obmm_cc_tx_slot_t *prev = NULL;
    uct_obmm_ep_t         *ep;

    if (!iface->cc.enabled || (ack->chunk_id >= iface->cc.chunk_count)) {
        ucs_warn("obmm: stale/invalid CC_ACK chunk=%u", ack->chunk_id);
        return;
    }

    for (tx_slot = iface->cc.tx_slots; tx_slot != NULL;
         prev = tx_slot, tx_slot = tx_slot->next) {
        if ((tx_slot->ready.seq == ack->seq) &&
            (tx_slot->ready.sender_generation == ack->sender_generation) &&
            (tx_slot->ready.chunk_id == ack->chunk_id)) {
            break;
        }
    }

    if (tx_slot == NULL) {
        ucs_trace_data("obmm: drop stale CC_ACK chunk=%u seq=%" PRIu64,
                       ack->chunk_id, ack->seq);
        return;
    }

    ep = tx_slot->ep;
    uct_obmm_iface_tx_list_remove(iface, tx_slot, prev);

    if (iface->cc.diag_enabled) {
        uct_obmm_cc_diag_bucket_t *diag =
            &iface->cc.diag[tx_slot->diag_bucket];
        uint64_t ack_wait_nsec =
            uct_obmm_cc_diag_nsec(ucs_get_time() - tx_slot->start_time);

        diag->tx_ack_count++;
        diag->tx_ack_wait_nsec += ack_wait_nsec;
        diag->tx_ack_wait_max_nsec = ucs_max(diag->tx_ack_wait_max_nsec,
                                             ack_wait_nsec);
    }

    if (iface->cc.outstanding > 0) {
        iface->cc.outstanding--;
    }
    if ((ep != NULL) && (ep->cc_outstanding > 0)) {
        ep->cc_outstanding--;
    }

    uct_obmm_cc_complete_flush(iface, ep);
    ucs_free(tx_slot);
}


static int
uct_obmm_cc_sender_slot_is_current(uct_obmm_iface_t *iface,
                                   const uct_obmm_cc_data_ready_t *ready)
{
    uct_obmm_md_t           *md = ucs_derived_of(iface->super.md,
                                                 uct_obmm_md_t);
    uct_obmm_eid_t           eid;
    uct_obmm_region_t       *region;
    uct_obmm_pool_t          pool;
    uct_obmm_slot_meta_t    *meta;
    uint32_t                 state;
    uint32_t                 generation;
    ucs_status_t             status;

    eid.hi = ready->sender_deid_hi;
    eid.lo = ready->sender_deid_lo;

    region = uct_obmm_md_find_region(md, UCT_OBMM_REGION_KIND_NC,
                                     ready->sender_dcna, &eid);
    if (region == NULL) {
        ucs_error("obmm: cannot validate CC sender slot; no NC region for "
                  "dcna=0x%" PRIx64 " deid=0x%" PRIx64 ":0x%" PRIx64,
                  ready->sender_dcna, ready->sender_deid_hi,
                  ready->sender_deid_lo);
        return 0;
    }

    status = uct_obmm_pool_open(region->base, region->length, &pool);
    if (status != UCS_OK) {
        ucs_trace_data("obmm: drop CC_DATA_READY from non-ready sender pool: "
                       "%s", ucs_status_string(status));
        return 0;
    }

    if ((ready->sender_slot_index >= pool.slot_count) ||
        (pool.slot_size != uct_obmm_slot_stride(iface->fifo_size,
                                                iface->fifo_elem_size,
                                                iface->bcopy_seg_size))) {
        ucs_warn("obmm: drop CC_DATA_READY with invalid sender pool geometry "
                 "slot=%u slot_count=%u slot_size=%u",
                 ready->sender_slot_index, pool.slot_count, pool.slot_size);
        return 0;
    }

    meta  = &pool.meta[ready->sender_slot_index];
    state = meta->state;
    ucs_memory_bus_load_fence();
    generation = meta->generation;

    if ((state != UCT_OBMM_SLOT_STATE_IN_USE) ||
        (generation != ready->sender_generation)) {
        ucs_trace_data("obmm: drop stale CC_DATA_READY slot=%u gen=%u "
                       "sender_gen=%u state=%u",
                       ready->sender_slot_index, generation,
                       ready->sender_generation, state);
        return 0;
    }

    return 1;
}


static void
uct_obmm_iface_queue_cc_ack(uct_obmm_iface_t *iface,
                            const uct_obmm_cc_data_ready_t *ready)
{
    uct_obmm_cc_pending_ack_t *ack;

    if (iface->cc.diag_enabled) {
        iface->cc.diag[uct_obmm_cc_diag_bucket_index(ready->length)].
            rx_ack_deferred++;
    }

    ack = ucs_malloc(sizeof(*ack), "obmm_cc_pending_ack");
    if (ack == NULL) {
        ucs_warn("obmm: failed to allocate pending CC ACK; sender zcopy "
                 "may remain outstanding");
        return;
    }

    ack->ready            = *ready;
    ack->next             = iface->cc.pending_acks;
    iface->cc.pending_acks = ack;
}


unsigned uct_obmm_iface_progress_cc_acks(uct_obmm_iface_t *iface)
{
    uct_obmm_cc_pending_ack_t **prev = &iface->cc.pending_acks;
    uct_obmm_cc_pending_ack_t  *ack;
    uct_obmm_cc_diag_bucket_t  *diag;
    ucs_time_t                  time_start = 0;
    ucs_status_t                status;
    unsigned                    count = 0;

    while (*prev != NULL) {
        ack  = *prev;
        diag = iface->cc.diag_enabled ?
               &iface->cc.diag[uct_obmm_cc_diag_bucket_index(
                                ack->ready.length)] : NULL;
        if (diag != NULL) {
            time_start = ucs_get_time();
        }
        status = uct_obmm_write_cc_ack(iface, &ack->ready);
        if (diag != NULL) {
            uct_obmm_cc_diag_add_time(&diag->rx_ack_nsec,
                                      ucs_get_time() - time_start);
        }
        if (status == UCS_ERR_NO_RESOURCE) {
            if (diag != NULL) {
                diag->rx_ack_nores++;
            }
            prev = &ack->next;
            continue;
        }

        *prev = ack->next;
        ucs_free(ack);
        if (status == UCS_OK) {
            ++count;
        } else {
            ucs_warn("obmm: dropping pending CC ACK after send failure: %s",
                     ucs_status_string(status));
        }
    }

    return count;
}


static void
uct_obmm_iface_release_cc_seq(uct_obmm_iface_t *iface, uint64_t cc_seq)
{
    uint64_t distance;
    uint64_t old_tail;
    uint64_t bit;

    if (cc_seq < iface->cc.rx_tail) {
        ucs_trace_data("obmm: ignore already released receiver CC seq=%" PRIu64
                       " tail=%" PRIu64, cc_seq, iface->cc.rx_tail);
        return;
    }

    distance = cc_seq - iface->cc.rx_tail;
    if (distance >= iface->cc.chunk_count) {
        ucs_warn("obmm: receiver CC seq outside active window: seq=%" PRIu64
                 " tail=%" PRIu64 " chunk_count=%u", cc_seq,
                 iface->cc.rx_tail, iface->cc.chunk_count);
        return;
    }

    bit = 1ull << distance;
    if (iface->cc.rx_done_mask & bit) {
        ucs_trace_data("obmm: duplicate receiver CC completion seq=%" PRIu64,
                       cc_seq);
        return;
    }

    old_tail = iface->cc.rx_tail;
    iface->cc.rx_done_mask |= bit;
    while (iface->cc.rx_done_mask & 1ull) {
        iface->cc.rx_done_mask >>= 1;
        iface->cc.rx_tail++;
    }

    if (iface->cc.rx_tail != old_tail) {
        uct_obmm_bus_full_fence();
        iface->recv_ctl->cc_tail = iface->cc.rx_tail;
    }
}


ucs_status_t
uct_obmm_iface_handle_cc_data_ready(uct_obmm_iface_t *iface, uint8_t am_id,
                                    const uct_obmm_cc_data_ready_t *ready)
{
    void              *chunk;
    size_t             own_length;
    uct_obmm_cc_diag_bucket_t *diag = NULL;
    uint64_t           gap;
    ucs_time_t         time_start = 0;
    ucs_status_t       status;

    if (!iface->cc.enabled ||
        (ready->sender_slot_index >= UCT_OBMM_POOL_SLOT_COUNT) ||
        (ready->chunk_id >= iface->cc.chunk_count) ||
        (ready->chunk_id !=
         (ready->receiver_cc_seq % iface->cc.chunk_count)) ||
        (ready->length > iface->cc.chunk_size)) {
        ucs_error("obmm: invalid CC_DATA_READY slot=%u chunk=%u "
                  "cc_seq=%" PRIu64 " length=%u", ready->sender_slot_index,
                  ready->chunk_id, ready->receiver_cc_seq, ready->length);
        return UCS_ERR_INVALID_PARAM;
    }
    own_length = uct_obmm_cc_ownership_length(iface, ready->length);
    if (own_length > iface->cc.chunk_size) {
        ucs_error("obmm: invalid CC_DATA_READY ownership length %zu "
                  "(payload=%u chunk=%zu)", own_length, ready->length,
                  iface->cc.chunk_size);
        return UCS_ERR_INVALID_PARAM;
    }

    if (iface->cc.diag_enabled) {
        diag = &iface->cc.diag[uct_obmm_cc_diag_bucket_index(ready->length)];
        diag->rx_calls++;
        diag->rx_bytes += ready->length;
        if (ready->receiver_cc_seq >= iface->cc.rx_tail) {
            gap = ready->receiver_cc_seq - iface->cc.rx_tail;
            if (gap > 0) {
                diag->rx_gap_count++;
                diag->rx_gap_max = ucs_max(diag->rx_gap_max, gap);
            }
        }
    }

    if (!uct_obmm_cc_sender_slot_is_current(iface, ready)) {
        uct_obmm_iface_release_cc_seq(iface, ready->receiver_cc_seq);
        return UCS_OK;
    }

    chunk = UCS_PTR_BYTE_OFFSET(
            iface->cc.slot_base,
            (size_t)ready->chunk_id * iface->cc.chunk_size);

    if (diag != NULL) {
        time_start = ucs_get_time();
    }
    status = uct_obmm_region_set_ownership(iface->cc.region, chunk, own_length,
                                           PROT_READ);
    if (diag != NULL) {
        uct_obmm_cc_diag_add_time(&diag->rx_read_own_nsec,
                                  ucs_get_time() - time_start);
    }
    if (status != UCS_OK) {
        return status;
    }

    if (diag != NULL) {
        time_start = ucs_get_time();
    }
    uct_iface_invoke_am(&iface->super, am_id, chunk, ready->length, 0);
    if (diag != NULL) {
        uct_obmm_cc_diag_add_time(&diag->rx_cb_nsec,
                                  ucs_get_time() - time_start);
    }

    if (diag != NULL) {
        time_start = ucs_get_time();
    }
    status = uct_obmm_region_set_ownership(iface->cc.region, chunk,
                                           own_length, PROT_NONE);
    if (diag != NULL) {
        uct_obmm_cc_diag_add_time(&diag->rx_none_own_nsec,
                                  ucs_get_time() - time_start);
    }
    if (status != UCS_OK) {
        ucs_warn("obmm: failed to release CC read ownership: %s",
                 ucs_status_string(status));
    }

    uct_obmm_iface_release_cc_seq(iface, ready->receiver_cc_seq);

    if (diag != NULL) {
        time_start = ucs_get_time();
    }
    status = uct_obmm_write_cc_ack(iface, ready);
    if (diag != NULL) {
        uct_obmm_cc_diag_add_time(&diag->rx_ack_nsec,
                                  ucs_get_time() - time_start);
    }
    if (status == UCS_ERR_NO_RESOURCE) {
        if (diag != NULL) {
            diag->rx_ack_nores++;
        }
        uct_obmm_iface_queue_cc_ack(iface, ready);
    } else if (status != UCS_OK) {
        ucs_warn("obmm: failed to send CC ACK: %s", ucs_status_string(status));
    }

    return UCS_OK;
}


void uct_obmm_iface_cleanup_cc(uct_obmm_iface_t *iface)
{
    uct_obmm_cc_pending_ack_t *ack, *next;
    uct_obmm_cc_tx_slot_t     *tx_slot, *tx_next;

    uct_obmm_cc_diag_print(iface);

    if (iface->cc.enabled && (iface->cc.outstanding > 0)) {
        ucs_warn("obmm: iface cleanup with %u outstanding CC zcopy chunks",
                 iface->cc.outstanding);
    }

    if (iface->cc.enabled && (iface->cc.slot_base != NULL)) {
        uct_obmm_region_set_ownership(iface->cc.region, iface->cc.slot_base,
                                      iface->cc.slot_stride, PROT_NONE);
    }

    for (ack = iface->cc.pending_acks; ack != NULL; ack = next) {
        next = ack->next;
        ucs_free(ack);
    }

    for (tx_slot = iface->cc.tx_slots; tx_slot != NULL; tx_slot = tx_next) {
        tx_next = tx_slot->next;
        ucs_free(tx_slot);
    }

    iface->cc.tx_slots     = NULL;
    iface->cc.pending_acks = NULL;
    iface->cc.enabled      = 0;
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


static UCS_F_ALWAYS_INLINE int
uct_obmm_ep_has_cc_resource(uct_obmm_ep_t *ep, uct_obmm_iface_t *iface)
{
    uint64_t head;

    if (!iface->cc.enabled) {
        return 1;
    }
    if (ep->peer_cc_region == NULL) {
        return 0;
    }

    head = ep->peer_ctl->cc_head;
    if ((head - ep->cached_cc_tail) < iface->cc.chunk_count) {
        return 1;
    }

    ucs_memory_bus_load_fence();
    ep->cached_cc_tail = ep->peer_ctl->cc_tail;
    return (head - ep->cached_cc_tail) < iface->cc.chunk_count;
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
        uct_obmm_ep_has_cc_resource(ep, iface) &&
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
