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
#include "obmm_ownership.h"

#include <uct/base/uct_log.h>
#include <uct/base/uct_iface.h>
#include <ucs/arch/cpu.h>
#include <ucs/debug/log.h>

#include <unistd.h>
#include <string.h>
#include <sys/mman.h>

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


static UCS_F_ALWAYS_INLINE void *
uct_obmm_ep_bulk_window(void *base, size_t window_size, unsigned window_index)
{
    return UCS_PTR_BYTE_OFFSET(base, (size_t)window_index * window_size);
}


static UCS_F_ALWAYS_INLINE int
uct_obmm_ep_bulk_desc_matches_local_iface(const uct_obmm_iface_t *iface,
                                          const uct_obmm_bulk_window_desc_t *desc)
{
    return (desc->target_slot_index == iface->slot_index) &&
           (desc->target_generation == iface->generation);
}


unsigned uct_obmm_iface_bulk_reclaim_windows(uct_obmm_iface_t *iface)
{
    uct_obmm_bulk_window_desc_t *desc;
    ucs_status_t                 status;
    unsigned                     reclaimed = 0;
    unsigned                     i;
    uint64_t                     seq;
    int                          needs_ownership;

    if (iface->role != UCT_OBMM_IFACE_ROLE_CC_BULK) {
        return 0;
    }

    for (i = 0; i < iface->bulk_window_count; ++i) {
        desc = &iface->bulk_ctrl_descs[i];
        seq  = desc->seq;
        if ((seq == 0) || (desc->sender_generation != iface->generation) ||
            (desc->ack_generation != iface->generation) ||
            (desc->ack_seq != seq)) {
            continue;
        }

        needs_ownership = !!(desc->flags &
                             UCT_OBMM_BULK_DESC_FLAG_REMOTE_OWNERSHIP);
        if (needs_ownership) {
            ucs_memory_bus_load_fence();
            status = uct_obmm_region_set_ownership(iface->data_region,
                                                   uct_obmm_ep_bulk_window(
                                                           iface->bulk_data_base,
                                                           iface->bulk_window_size,
                                                           i),
                                                   iface->bulk_window_size,
                                                   PROT_WRITE);
            if (status != UCS_OK) {
                return reclaimed;
            }
        }

        desc->ack_seq           = 0;
        desc->cc_memid          = 0;
        desc->length            = 0;
        desc->target_slot_index = 0;
        desc->target_generation = 0;
        desc->am_id             = 0;
        desc->flags             = 0;
        desc->sender_generation = 0;
        desc->ack_generation    = 0;
        ucs_memory_bus_store_fence();
        desc->seq               = 0;
        ++reclaimed;
    }

    return reclaimed;
}


static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_bulk_find_window(uct_obmm_iface_t *iface, unsigned *window_index_p)
{
    uct_obmm_bulk_window_desc_t *desc;
    unsigned                     i;
    unsigned                     index;

    uct_obmm_iface_bulk_reclaim_windows(iface);
    for (i = 0; i < iface->bulk_window_count; ++i) {
        index = (iface->bulk_next_window + i) % iface->bulk_window_count;
        desc  = &iface->bulk_ctrl_descs[index];
        if (desc->seq == 0) {
            *window_index_p = index;
            iface->bulk_next_window = (index + 1) % iface->bulk_window_count;
            return UCS_OK;
        }
    }

    return UCS_ERR_NO_RESOURCE;
}


static unsigned
uct_obmm_ep_progress_bulk_one(uct_obmm_ep_t *ep, uct_obmm_iface_t *iface)
{
    uct_obmm_bulk_window_desc_t *desc;
    uct_obmm_bulk_window_desc_t *candidate_desc = NULL;
    void                        *window;
    ucs_status_t                 status;
    uint32_t                     observed_generation;
    uint64_t                     publish_seq;
    uint64_t                     candidate_seq = UINT64_MAX;
    int                          needs_ownership;
    unsigned                     i;
    unsigned                     candidate_index = 0;

    observed_generation = ep->peer_bulk_ctrl->generation;
    publish_seq         = ep->peer_bulk_ctrl->req_seq;
    if (observed_generation != ep->expected_generation) {
        return 0;
    }
    if (publish_seq <= ep->bulk_last_seen_seq) {
        return 0;
    }

    ucs_memory_bus_load_fence();
    for (i = 0; i < ep->bulk_window_count; ++i) {
        desc = &ep->peer_bulk_descs[i];
        if ((desc->seq <= ep->bulk_last_seen_seq) || (desc->seq > publish_seq)) {
            continue;
        }
        if (!uct_obmm_ep_bulk_desc_matches_local_iface(iface, desc)) {
            continue;
        }
        if (desc->cc_memid != ep->peer_cc_memid) {
            continue;
        }
        if (desc->sender_generation != observed_generation) {
            continue;
        }
        if (desc->seq < candidate_seq) {
            candidate_seq   = desc->seq;
            candidate_desc  = desc;
            candidate_index = i;
        }
    }

    if (candidate_desc == NULL) {
        return 0;
    }
    if (candidate_desc->length > ep->bulk_window_size) {
        ucs_error("obmm_bulk: invalid bulk length %u for seq=%lu "
                  "(window_size=%zu)",
                  candidate_desc->length, (unsigned long)candidate_desc->seq,
                  ep->bulk_window_size);
        return 0;
    }

    needs_ownership = !!(candidate_desc->flags &
                         UCT_OBMM_BULK_DESC_FLAG_REMOTE_OWNERSHIP);
    window = uct_obmm_ep_bulk_window(ep->peer_bulk_data_base, ep->bulk_window_size,
                                     candidate_index);
    if (needs_ownership) {
        status = uct_obmm_region_set_ownership(ep->peer_bulk_data_region, window,
                                               ep->bulk_window_size, PROT_READ);
        if (status != UCS_OK) {
            return 0;
        }
    }

    ucs_memory_cpu_load_fence();
    uct_iface_invoke_am(&iface->super, candidate_desc->am_id, window,
                        candidate_desc->length, 0);
    /* The callback succeeded, so never deliver this descriptor again even if
     * the subsequent ownership release fails. The sender will not see an ACK in
     * that case, so the affected window remains stuck rather than being reused
     * behind upper-layer data that was already consumed once. */
    ep->bulk_last_seen_seq = candidate_seq;
    if (needs_ownership) {
        status = uct_obmm_region_set_ownership(ep->peer_bulk_data_region, window,
                                               ep->bulk_window_size, PROT_NONE);
        if (status != UCS_OK) {
            return 0;
        }
    }

    uct_obmm_bus_full_fence();
    candidate_desc->ack_generation = observed_generation;
    candidate_desc->ack_seq        = candidate_desc->seq;
    return 1;
}


unsigned uct_obmm_ep_progress_bulk_rx(uct_obmm_ep_t *ep, uct_obmm_iface_t *iface,
                                      unsigned max_poll)
{
    unsigned polled = 0;

    while (polled < max_poll) {
        if (uct_obmm_ep_progress_bulk_one(ep, iface) == 0) {
            break;
        }
        ++polled;
    }

    return polled;
}


static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_am_short_spsc(uct_obmm_ep_t *ep, uct_obmm_iface_t *iface,
                          uint8_t id, uint64_t header, const void *payload,
                          unsigned length, size_t payload_total)
{
    uct_obmm_short_lane_t    *lane = ep->short_lane;
    uct_obmm_fifo_element_t  *elem;
    uint64_t                  head = ep->short_lane_head;

    if (!ep->short_lane_active) {
        uct_obmm_ep_short_lane_activate(ep->short_lane_active_mask_p,
                                        ep->short_lane_index);
        ep->short_lane_active = 1;
    }

    if ((head - ep->short_lane_cached_tail) >= UCT_OBMM_SHORT_LANE_FIFO_SIZE) {
        ucs_memory_bus_load_fence();
        ep->short_lane_cached_tail = lane->ctl.tail;
        if ((head - ep->short_lane_cached_tail) >=
            UCT_OBMM_SHORT_LANE_FIFO_SIZE) {
            return UCS_ERR_NO_RESOURCE;
        }
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

    ucs_memory_bus_store_fence();
    lane->ctl.head      = head + 1;
    ep->short_lane_head = head + 1;
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
    uct_obmm_region_t            *data_region;
    uct_obmm_pool_t               peer_pool;
    void                         *peer_slot;
    void                         *peer_pool_base;
    size_t                        peer_pool_length;
    void                         *peer_bulk_data_base;
    size_t                        peer_bulk_data_offset;
    size_t                        peer_bulk_data_length;
    ucs_status_t                  status;
    uct_obmm_map_mode_t           map_mode;
    uct_obmm_eid_t                eid;

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);

    ucs_arbiter_group_init(&self->arb_group);
    ucs_list_head_init(&self->list);
    self->peer_ctl              = NULL;
    self->peer_elems            = NULL;
    self->peer_descs            = NULL;
    self->cached_tail           = 0;
    self->expected_generation   = 0;
    self->fifo_size             = 0;
    self->fifo_mask             = 0;
    self->fifo_elem_size        = 0;
    self->bcopy_seg_size        = 0;
    self->short_lane_active_mask_p = NULL;
    self->short_lane            = NULL;
    self->short_lane_index      = 0;
    self->short_lane_head       = 0;
    self->short_lane_cached_tail = 0;
    self->short_lane_active     = 0;
    self->peer_cc_memid         = 0;
    self->peer_slot             = NULL;
    self->peer_bulk_data_region = NULL;
    self->peer_bulk_data_base   = NULL;
    self->peer_bulk_ctrl        = NULL;
    self->peer_bulk_descs       = NULL;
    self->bulk_window_size      = 0;
    self->bulk_window_count     = 0;
    self->bulk_last_seen_seq    = 0;
    self->bulk_peer_local       = 0;

    daddr = (const uct_obmm_device_addr_t*)params->dev_addr;
    iaddr = (const uct_obmm_iface_addr_t*)params->iface_addr;
    map_mode = uct_obmm_iface_role_map_mode(iface->role);
    eid.hi = daddr->exporter_deid_hi;
    eid.lo = daddr->exporter_deid_lo;

    /* Reject incompatible geometry. UCX wireup should already have filtered
     * this out via is_reachable_v2, but double-check. */
    if (iaddr->role != iface->role) {
        ucs_error("obmm: peer role %u does not match local role %u",
                  iaddr->role, iface->role);
        return UCS_ERR_UNREACHABLE;
    }
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
    if ((iface->role == UCT_OBMM_IFACE_ROLE_CC_BULK) &&
        ((iaddr->bulk_window_size != iface->bulk_window_size) ||
         (iaddr->bulk_window_count != iface->bulk_window_count) ||
         (iaddr->bulk_data_offset != iface->bulk_data_offset) ||
         (iaddr->bulk_cc_memid == 0))) {
        ucs_error("obmm: peer bulk geometry differs from local "
                  "(peer window=%zu/%u offset=%u memid=%lu local window=%zu/%u "
                  "offset=%zu)",
                  (size_t)iaddr->bulk_window_size, iaddr->bulk_window_count,
                  iaddr->bulk_data_offset,
                  (unsigned long)iaddr->bulk_cc_memid, iface->bulk_window_size,
                  iface->bulk_window_count, iface->bulk_data_offset);
        return UCS_ERR_UNREACHABLE;
    }

    /* Find the local region (export-for-self, import-for-remote) that maps
     * the peer's exporter coordinates. */
    region      = NULL;
    data_region = NULL;
    if (iface->role == UCT_OBMM_IFACE_ROLE_CC_BULK) {
        uct_obmm_region_t *exp_r;

        exp_r = uct_obmm_md_export_region_by_mode(md, UCT_OBMM_MAP_MODE_NC);
        if ((exp_r != NULL) &&
            (exp_r->info.exporter_dcna == daddr->exporter_dcna) &&
            (exp_r->info.exporter_deid.hi == eid.hi) &&
            (exp_r->info.exporter_deid.lo == eid.lo)) {
            region = exp_r;
        } else {
            region = uct_obmm_md_find_import_region_by_mode(md, UCT_OBMM_MAP_MODE_NC,
                                                            daddr->exporter_dcna,
                                                            &eid);
        }

        exp_r = uct_obmm_md_export_region_by_mode(md, UCT_OBMM_MAP_MODE_CC);
        if ((exp_r != NULL) && (exp_r->info.memid == iaddr->bulk_cc_memid) &&
            (exp_r->info.exporter_dcna == daddr->exporter_dcna) &&
            (exp_r->info.exporter_deid.hi == eid.hi) &&
            (exp_r->info.exporter_deid.lo == eid.lo)) {
            data_region = exp_r;
        } else {
            data_region = uct_obmm_md_find_import_region_by_mode_memid(
                    md, UCT_OBMM_MAP_MODE_CC, daddr->exporter_dcna, &eid,
                    iaddr->bulk_cc_memid);
        }
    } else {
        uct_obmm_region_t *exp_r;

        exp_r  = uct_obmm_md_export_region_by_mode(md, map_mode);
        if ((exp_r != NULL) &&
            (exp_r->info.exporter_dcna == daddr->exporter_dcna) &&
            (exp_r->info.exporter_deid.hi == eid.hi) &&
            (exp_r->info.exporter_deid.lo == eid.lo)) {
            region = exp_r;
        } else if (iface->role == UCT_OBMM_IFACE_ROLE_NC_REMOTE) {
            region = uct_obmm_md_find_import_region_by_mode(md, map_mode,
                                                            daddr->exporter_dcna,
                                                            &eid);
        }
    }
    if (region == NULL) {
        ucs_error("obmm: ep_create cannot find %s region for peer "
                  "dcna=0x%lx deid=0x%lx:0x%lx",
                  (iface->role == UCT_OBMM_IFACE_ROLE_CC_LOCAL) ?
                  "cc_local" :
                  ((iface->role == UCT_OBMM_IFACE_ROLE_CC_BULK) ?
                   "cc_bulk_control" : "nc_remote"),
                  (unsigned long)daddr->exporter_dcna,
                  (unsigned long)daddr->exporter_deid_hi,
                  (unsigned long)daddr->exporter_deid_lo);
        return UCS_ERR_UNREACHABLE;
    }
    if ((iface->role == UCT_OBMM_IFACE_ROLE_CC_BULK) && (data_region == NULL)) {
        ucs_error("obmm: ep_create cannot find cc_bulk data region for peer "
                  "dcna=0x%lx deid=0x%lx:0x%lx memid=%lu",
                  (unsigned long)daddr->exporter_dcna,
                  (unsigned long)daddr->exporter_deid_hi,
                  (unsigned long)daddr->exporter_deid_lo,
                  (unsigned long)iaddr->bulk_cc_memid);
        return UCS_ERR_UNREACHABLE;
    }

    if (iface->role == UCT_OBMM_IFACE_ROLE_CC_LOCAL) {
        status = uct_obmm_cc_local_pool_region(region->base, region->length,
                                               &peer_pool_base,
                                               &peer_pool_length);
        if (status != UCS_OK) {
            ucs_error("obmm: peer CC region memid=%lu is smaller than the "
                      "computed obmm_cc short-only prefix",
                      (unsigned long)region->info.memid);
            return status;
        }
    } else {
        peer_pool_base   = region->base;
        peer_pool_length = region->length;
    }

    status = uct_obmm_pool_open(peer_pool_base, peer_pool_length, &peer_pool);
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
    self->expected_generation = iaddr->generation;
    self->fifo_size           = iaddr->fifo_size;
    self->fifo_mask           = iaddr->fifo_size - 1u;
    self->fifo_elem_size      = iaddr->fifo_elem_size;
    self->bcopy_seg_size      = iaddr->bcopy_seg_size;
    self->peer_dcna           = daddr->exporter_dcna;
    self->peer_deid_hi        = daddr->exporter_deid_hi;
    self->peer_deid_lo        = daddr->exporter_deid_lo;
    self->peer_slot_index     = iaddr->slot_index;
    self->peer_cc_memid       = iaddr->bulk_cc_memid;
    self->peer_slot           = peer_slot;
    if (iface->role == UCT_OBMM_IFACE_ROLE_CC_BULK) {
        self->bulk_peer_local = uct_obmm_ep_short_lane_is_local_sender(iface,
                                                                       daddr);
        if (uct_obmm_cc_bulk_data_region_split(data_region->base,
                                               data_region->length,
                                               &peer_bulk_data_base,
                                               &peer_bulk_data_offset,
                                               &peer_bulk_data_length) != UCS_OK) {
            ucs_error("obmm: peer CC bulk region memid=%lu has no space after "
                      "the computed obmm_cc short-only prefix",
                      (unsigned long)data_region->info.memid);
            return UCS_ERR_UNREACHABLE;
        }
        if (((size_t)iaddr->bulk_window_count * iaddr->bulk_window_size) >
            peer_bulk_data_length) {
            ucs_error("obmm: peer cc_bulk arena is too small for the advertised "
                      "window geometry (%u * %zu > %zu)",
                      iaddr->bulk_window_count,
                      (size_t)iaddr->bulk_window_size, peer_bulk_data_length);
            return UCS_ERR_UNREACHABLE;
        }
        (void)peer_bulk_data_offset;

        self->peer_bulk_ctrl        = uct_obmm_bulk_ctrl_hdr(peer_slot);
        self->peer_bulk_descs       = uct_obmm_bulk_ctrl_descs(peer_slot);
        self->peer_bulk_data_region = data_region;
        self->peer_bulk_data_base   = peer_bulk_data_base;
        self->bulk_window_size      = iaddr->bulk_window_size;
        self->bulk_window_count     = iaddr->bulk_window_count;
        self->bulk_last_seen_seq    = 0;
        if ((self->peer_bulk_ctrl->magic != UCT_OBMM_BULK_CTRL_MAGIC) ||
            (self->peer_bulk_ctrl->generation != iaddr->generation) ||
            (self->peer_bulk_ctrl->version != UCT_OBMM_BULK_CTRL_VERSION) ||
            (self->peer_bulk_ctrl->cc_memid != iaddr->bulk_cc_memid) ||
            (self->peer_bulk_ctrl->window_size != iaddr->bulk_window_size) ||
            (self->peer_bulk_ctrl->window_count != iaddr->bulk_window_count)) {
            ucs_error("obmm_bulk: peer control slot header mismatch "
                      "(magic=0x%x gen=%u ver=%u cc_memid=%lu window=%zu/%u "
                      "expected magic=0x%x gen=%u ver=%u cc_memid=%lu "
                      "window=%zu/%u)",
                      self->peer_bulk_ctrl->magic,
                      self->peer_bulk_ctrl->generation,
                      self->peer_bulk_ctrl->version,
                      (unsigned long)self->peer_bulk_ctrl->cc_memid,
                      (size_t)self->peer_bulk_ctrl->window_size,
                      self->peer_bulk_ctrl->window_count,
                      UCT_OBMM_BULK_CTRL_MAGIC,
                      iaddr->generation,
                      UCT_OBMM_BULK_CTRL_VERSION,
                      (unsigned long)iaddr->bulk_cc_memid,
                      (size_t)iaddr->bulk_window_size,
                      iaddr->bulk_window_count);
            return UCS_ERR_UNREACHABLE;
        }
        ucs_list_add_tail(&iface->ep_list, &self->list);
    } else {
        self->peer_ctl            = uct_obmm_slot_ctl(peer_slot);
        self->peer_elems          = uct_obmm_slot_elems(peer_slot);
        self->peer_descs          = uct_obmm_slot_descs(peer_slot,
                                                        iaddr->fifo_size,
                                                        iaddr->fifo_elem_size);
        self->cached_tail         = self->peer_ctl->tail;
        uct_obmm_ep_init_short_lane(self, iface, daddr, peer_slot);
    }
    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_obmm_ep_t)
{
    if (((uct_obmm_iface_t*)self->super.super.iface)->role ==
        UCT_OBMM_IFACE_ROLE_CC_BULK) {
        ucs_list_del(&self->list);
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
           (iaddr->role == ((uct_obmm_iface_t*)tl_ep->iface)->role) &&
           (iaddr->slot_index == ep->peer_slot_index) &&
           (iaddr->generation == ep->expected_generation) &&
           (iaddr->bulk_window_size == ep->bulk_window_size) &&
           (iaddr->bulk_window_count == ep->bulk_window_count) &&
           (iaddr->bulk_cc_memid == ep->peer_cc_memid);
}


ucs_status_t uct_obmm_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t header,
                                  const void *payload, unsigned length)
{
    uct_obmm_ep_t           *ep    = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    uct_obmm_iface_t        *iface = ucs_derived_of(tl_ep->iface,
                                                    uct_obmm_iface_t);
    size_t                   payload_total = sizeof(header) + length;

    if (iface->role == UCT_OBMM_IFACE_ROLE_CC_BULK) {
        return UCS_ERR_UNSUPPORTED;
    }

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


ssize_t uct_obmm_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                             uct_pack_callback_t pack_cb, void *arg,
                             unsigned flags)
{
    uct_obmm_ep_t           *ep    = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    uct_obmm_iface_t        *iface = ucs_derived_of(tl_ep->iface,
                                                    uct_obmm_iface_t);
    uct_obmm_fifo_element_t *elem;
    uct_obmm_bulk_window_desc_t *bulk_desc;
    void                    *desc;
    void                    *window;
    uint64_t                 head;
    uint64_t                 seq;
    size_t                   length;
    uint8_t                  owner_bit;
    ucs_status_t             status;
    unsigned                 window_index;

    /* flags (UCT_SEND_FLAG_PEER_CHECK etc.) are ignored: this transport
     * does not advertise EP_CHECK / keepalive in v1. */
    (void)flags;

    UCT_CHECK_AM_ID(id);

    if (iface->role == UCT_OBMM_IFACE_ROLE_CC_BULK) {
        int use_ownership = !ep->bulk_peer_local;

        status = uct_obmm_ep_bulk_find_window(iface, &window_index);
        if (status != UCS_OK) {
            return status;
        }

        bulk_desc = &iface->bulk_ctrl_descs[window_index];
        window    = uct_obmm_ep_bulk_window(iface->bulk_data_base,
                                            iface->bulk_window_size,
                                            window_index);
        length    = pack_cb(window, arg);
        ucs_assertv(length <= iface->bulk_window_size,
                    "obmm_bulk: pack_cb returned %zu > window_size=%zu",
                    length, iface->bulk_window_size);

        ucs_memory_cpu_store_fence();
        if (use_ownership) {
            status = uct_obmm_region_set_ownership(iface->data_region, window,
                                                   iface->bulk_window_size,
                                                   PROT_READ);
            if (status != UCS_OK) {
                return status;
            }
        }

        seq = iface->bulk_ctrl_hdr->req_seq + 1;
        bulk_desc->ack_seq           = 0;
        bulk_desc->cc_memid          = iface->data_region->info.memid;
        bulk_desc->length            = (uint32_t)length;
        bulk_desc->target_slot_index = ep->peer_slot_index;
        bulk_desc->target_generation = ep->expected_generation;
        bulk_desc->am_id             = id;
        bulk_desc->flags             = use_ownership ?
                                       UCT_OBMM_BULK_DESC_FLAG_REMOTE_OWNERSHIP : 0;
        bulk_desc->sender_generation = iface->generation;
        bulk_desc->ack_generation    = 0;
        bulk_desc->seq               = seq;
        ucs_memory_bus_store_fence();
        iface->bulk_ctrl_hdr->req_seq = seq;

        UCT_TL_EP_STAT_OP(&ep->super, AM, BCOPY, length);
        uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, id,
                           window, length, "TX: AM_BCOPY_CC_BULK");
        return (ssize_t)length;
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

    owner_bit = (head & ep->fifo_size) ? 0u :
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


/* Returns true iff the peer's FIFO has at least one free slot, refreshing
 * cached_tail (with a bus_load_fence pair) before declaring "full". Mirrors
 * the resource check used by mm in pending_add. */
static UCS_F_ALWAYS_INLINE int
uct_obmm_ep_has_tx_resource(uct_obmm_ep_t *ep)
{
    uct_obmm_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                             uct_obmm_iface_t);
    uint64_t head;
    unsigned window_index;

    if (iface->role == UCT_OBMM_IFACE_ROLE_CC_BULK) {
        return uct_obmm_ep_bulk_find_window(iface, &window_index) == UCS_OK;
    }

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
