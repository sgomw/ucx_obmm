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
#include <string.h>

static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_reserve_slot(uct_obmm_ep_t *ep, uint64_t *head_p);

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
    uct_obmm_pool_t               peer_pool;
    void                         *peer_slot;
    ucs_status_t                  status;
    uint32_t                      expected_wire_format;

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);

    ucs_arbiter_group_init(&self->arb_group);
    self->cc_outstanding = 0;
    self->cc_flush_comp  = NULL;

    daddr = (const uct_obmm_device_addr_t*)params->dev_addr;
    iaddr = (const uct_obmm_iface_addr_t*)params->iface_addr;

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
         (iface->cc.enabled ? iface->cc.min_zcopy : 0))) {
        ucs_error("obmm: peer geometry (slots=%u lanes=%u wire=%u fifo=%u "
                  "elem=%u seg=%u cc_count=%u cc_size=%u cc_min=%u) differs "
                  "from local (slots=%u lanes=%u wire=%u fifo=%u elem=%u "
                  "seg=%u cc_count=%u cc_size=%zu cc_min=%zu); ep_create "
                  "rejected",
                  iaddr->slot_count, iaddr->short_lane_count,
                  iaddr->wire_format, iaddr->fifo_size, iaddr->fifo_elem_size,
                  iaddr->bcopy_seg_size, iaddr->cc_chunk_count,
                  iaddr->cc_chunk_size, iaddr->cc_min_zcopy,
                  UCT_OBMM_POOL_SLOT_COUNT, UCT_OBMM_SHORT_LANE_COUNT,
                  expected_wire_format,
                  iface->fifo_size, iface->fifo_elem_size,
                  iface->bcopy_seg_size,
                  iface->cc.enabled ? iface->cc.chunk_count : 0,
                  iface->cc.enabled ? iface->cc.chunk_size : 0,
                  iface->cc.enabled ? iface->cc.min_zcopy : 0);
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

    if (iface->cc.enabled &&
        (uct_obmm_md_find_region(md, UCT_OBMM_REGION_KIND_CC,
                                 daddr->exporter_dcna, &eid) == NULL)) {
        ucs_error("obmm: ep_create cannot find CC region for peer "
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


static int uct_obmm_cc_find_free_chunk(uct_obmm_iface_t *iface)
{
    unsigned i;

    if (iface->cc.free_mask == 0) {
        return -1;
    }

    for (i = 0; i < iface->cc.chunk_count; ++i) {
        if (iface->cc.free_mask & (1ull << i)) {
            return (int)i;
        }
    }

    return -1;
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
    int                       chunk_id;
    uint64_t                  chunk_bit;
    ucs_status_t              status;

    (void)flags;
    (void)comp;

    if (!iface->cc.enabled) {
        ucs_debug("obmm: am_zcopy rejected on ep=%p id=%u: CC disabled",
                  ep, id);
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

    ucs_debug("obmm: am_zcopy called ep=%p id=%u header=%u payload=%zu "
              "total=%zu iovcnt=%zu cc_min=%zu cc_chunk=%zu "
              "cc_max_iov=%u free_mask=0x%" PRIx64 " outstanding=%u",
              ep, id, header_length, payload_length, total_length, iovcnt,
              iface->cc.min_zcopy, iface->cc.chunk_size, iface->cc.max_iov,
              iface->cc.free_mask, iface->cc.outstanding);

    chunk_id = uct_obmm_cc_find_free_chunk(iface);
    if (chunk_id < 0) {
        ucs_debug("obmm: am_zcopy no free CC chunk ep=%p total=%zu "
                  "free_mask=0x%" PRIx64 " outstanding=%u",
                  ep, total_length, iface->cc.free_mask,
                  iface->cc.outstanding);
        UCS_STATS_UPDATE_COUNTER(ep->super.stats, UCT_EP_STAT_NO_RES, 1);
        return UCS_ERR_NO_RESOURCE;
    }

    chunk_bit = 1ull << (unsigned)chunk_id;
    iface->cc.free_mask &= ~chunk_bit;
    chunk = UCS_PTR_BYTE_OFFSET(iface->cc.slot_base,
                                (size_t)chunk_id * iface->cc.chunk_size);

    status = uct_obmm_region_set_ownership(iface->cc.region, chunk,
                                           iface->cc.chunk_size, PROT_WRITE);
    if (status != UCS_OK) {
        ucs_debug("obmm: am_zcopy PROT_WRITE ownership failed ep=%p "
                  "chunk=%d length=%zu status=%s",
                  ep, chunk_id, iface->cc.chunk_size,
                  ucs_status_string(status));
        iface->cc.free_mask |= chunk_bit;
        return status;
    }

    if (header_length > 0) {
        memcpy(chunk, header, header_length);
    }
    uct_obmm_cc_copy_iov(UCS_PTR_BYTE_OFFSET(chunk, header_length), iov,
                         iovcnt);

    status = uct_obmm_region_set_ownership(iface->cc.region, chunk,
                                           iface->cc.chunk_size, PROT_NONE);
    if (status != UCS_OK) {
        ucs_debug("obmm: am_zcopy PROT_NONE ownership failed ep=%p "
                  "chunk=%d length=%zu status=%s",
                  ep, chunk_id, iface->cc.chunk_size,
                  ucs_status_string(status));
        return status;
    }

    ready.sender_dcna        = iface->region->info.exporter_dcna;
    ready.sender_deid_hi     = iface->region->info.exporter_deid.hi;
    ready.sender_deid_lo     = iface->region->info.exporter_deid.lo;
    ready.seq                = iface->cc.next_seq++;
    ready.sender_slot_index  = iface->slot_index;
    ready.sender_generation  = iface->generation;
    ready.chunk_id           = (uint32_t)chunk_id;
    ready.length             = (uint32_t)total_length;

    status = uct_obmm_ep_send_cc_data_ready(ep, &ready, id);
    if (status != UCS_OK) {
        ucs_debug("obmm: am_zcopy failed to send CC_DATA_READY ep=%p "
                  "chunk=%d seq=%" PRIu64 " status=%s",
                  ep, chunk_id, ready.seq, ucs_status_string(status));
        iface->cc.free_mask |= chunk_bit;
        return status;
    }

    tx_slot         = &iface->cc.tx_slots[chunk_id];
    tx_slot->seq    = ready.seq;
    tx_slot->ep     = ep;
    tx_slot->in_use = 1;
    iface->cc.outstanding++;
    ep->cc_outstanding++;

    UCT_TL_EP_STAT_OP(&ep->super, AM, ZCOPY, total_length);
    ucs_debug("obmm: am_zcopy posted ep=%p chunk=%d seq=%" PRIu64
              " total=%zu outstanding iface=%u ep=%u",
              ep, chunk_id, ready.seq, total_length, iface->cc.outstanding,
              ep->cc_outstanding);
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
    uct_obmm_ep_t         *ep;
    uint64_t               chunk_bit;

    if (!iface->cc.enabled || (ack->chunk_id >= iface->cc.chunk_count)) {
        ucs_warn("obmm: stale/invalid CC_ACK chunk=%u", ack->chunk_id);
        return;
    }

    tx_slot = &iface->cc.tx_slots[ack->chunk_id];
    if (!tx_slot->in_use || (tx_slot->seq != ack->seq) ||
        (ack->sender_generation != iface->generation)) {
        ucs_trace_data("obmm: drop stale CC_ACK chunk=%u seq=%" PRIu64,
                       ack->chunk_id, ack->seq);
        return;
    }

    ep        = tx_slot->ep;
    chunk_bit = 1ull << ack->chunk_id;

    tx_slot->in_use = 0;
    tx_slot->seq    = 0;
    tx_slot->ep     = NULL;
    iface->cc.free_mask |= chunk_bit;

    if (iface->cc.outstanding > 0) {
        iface->cc.outstanding--;
    }
    if ((ep != NULL) && (ep->cc_outstanding > 0)) {
        ep->cc_outstanding--;
    }

    uct_obmm_cc_complete_flush(iface, ep);
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

    ack = ucs_malloc(sizeof(*ack), "obmm_cc_pending_ack");
    if (ack == NULL) {
        ucs_warn("obmm: failed to allocate pending CC ACK; sender chunk "
                 "may remain busy");
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
    ucs_status_t                status;
    unsigned                    count = 0;

    while (*prev != NULL) {
        ack    = *prev;
        status = uct_obmm_write_cc_ack(iface, &ack->ready);
        if (status == UCS_ERR_NO_RESOURCE) {
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


ucs_status_t
uct_obmm_iface_handle_cc_data_ready(uct_obmm_iface_t *iface, uint8_t am_id,
                                    const uct_obmm_cc_data_ready_t *ready)
{
    uct_obmm_md_t     *md = ucs_derived_of(iface->super.md, uct_obmm_md_t);
    uct_obmm_eid_t     eid;
    uct_obmm_region_t *region;
    void              *chunk;
    ucs_status_t       status;

    if (!iface->cc.enabled ||
        (ready->sender_slot_index >= UCT_OBMM_POOL_SLOT_COUNT) ||
        (ready->chunk_id >= iface->cc.chunk_count) ||
        (ready->length > iface->cc.chunk_size)) {
        ucs_error("obmm: invalid CC_DATA_READY slot=%u chunk=%u length=%u",
                  ready->sender_slot_index, ready->chunk_id, ready->length);
        return UCS_ERR_INVALID_PARAM;
    }

    ucs_debug("obmm: CC_DATA_READY am_id=%u slot=%u chunk=%u seq=%" PRIu64
              " length=%u cc_min=%zu cc_chunk=%zu",
              am_id, ready->sender_slot_index, ready->chunk_id, ready->seq,
              ready->length, iface->cc.min_zcopy, iface->cc.chunk_size);

    if (!uct_obmm_cc_sender_slot_is_current(iface, ready)) {
        return UCS_OK;
    }

    eid.hi = ready->sender_deid_hi;
    eid.lo = ready->sender_deid_lo;

    region = uct_obmm_md_find_region(md, UCT_OBMM_REGION_KIND_CC,
                                     ready->sender_dcna, &eid);
    if (region == NULL) {
        ucs_error("obmm: no CC region for sender dcna=0x%" PRIx64
                  " deid=0x%" PRIx64 ":0x%" PRIx64,
                  ready->sender_dcna, ready->sender_deid_hi,
                  ready->sender_deid_lo);
        return UCS_ERR_UNREACHABLE;
    }

    chunk = UCS_PTR_BYTE_OFFSET(
            region->base,
            ((size_t)ready->sender_slot_index * iface->cc.slot_stride) +
            ((size_t)ready->chunk_id * iface->cc.chunk_size));

    status = uct_obmm_region_set_ownership(region, chunk,
                                           iface->cc.chunk_size, PROT_READ);
    if (status != UCS_OK) {
        return status;
    }

    uct_iface_invoke_am(&iface->super, am_id, chunk, ready->length, 0);

    status = uct_obmm_region_set_ownership(region, chunk,
                                           iface->cc.chunk_size, PROT_NONE);
    if (status != UCS_OK) {
        ucs_warn("obmm: failed to release CC read ownership: %s",
                 ucs_status_string(status));
    }

    status = uct_obmm_write_cc_ack(iface, ready);
    if (status == UCS_ERR_NO_RESOURCE) {
        uct_obmm_iface_queue_cc_ack(iface, ready);
    } else if (status != UCS_OK) {
        ucs_warn("obmm: failed to send CC ACK: %s", ucs_status_string(status));
    }

    return UCS_OK;
}


void uct_obmm_iface_cleanup_cc(uct_obmm_iface_t *iface)
{
    uct_obmm_cc_pending_ack_t *ack, *next;

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

    ucs_free(iface->cc.tx_slots);
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
        (!iface->cc.enabled || (iface->cc.free_mask != 0)) &&
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
