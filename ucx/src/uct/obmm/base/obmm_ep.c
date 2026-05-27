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
uct_obmm_ep_reserve_slot(uct_obmm_ep_eager_path_t *path, uct_base_ep_t *ep,
                         uint64_t *head_p);

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


static UCS_F_ALWAYS_INLINE void
uct_obmm_ep_init_short_lane(uct_obmm_ep_eager_path_t *path,
                            uint32_t local_slot_index,
                            uint32_t local_generation,
                            void *peer_slot, int is_local_sender)
{
    uct_obmm_short_lane_t *lane;
    unsigned               lane_index;

    lane_index = local_slot_index;
    if (!is_local_sender) {
        lane_index += UCT_OBMM_POOL_SLOT_COUNT;
    }

    lane          = uct_obmm_slot_short_lane(peer_slot, lane_index);
    path->short_lane.active_mask_p = uct_obmm_slot_short_active_mask(peer_slot);
    if ((lane->meta.sender_slot_index != local_slot_index) ||
        (lane->meta.sender_generation != local_generation) ||
        (lane->meta.sender_pid != (uint32_t)getpid())) {
        lane->meta.sender_slot_index = local_slot_index;
        lane->meta.sender_generation = local_generation;
        lane->meta.sender_pid        = (uint32_t)getpid();
        lane->ctl.head               = 0;
        lane->ctl.tail               = 0;
        ucs_memory_bus_store_fence();
    }
    path->short_lane.lane        = lane;
    path->short_lane.lane_index  = lane_index;
    path->short_lane.head        = lane->ctl.head;
    path->short_lane.cached_tail = lane->ctl.tail;
    path->short_lane.active      = 0;
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
    return (desc->target_slot_index == iface->bulk.ctrl_slot_index) &&
           (desc->target_generation == iface->bulk.ctrl_generation);
}


unsigned uct_obmm_iface_bulk_reclaim_windows(uct_obmm_iface_t *iface)
{
    uct_obmm_bulk_window_desc_t *desc;
    ucs_status_t                 status;
    unsigned                     reclaimed = 0;
    unsigned                     i;
    uint64_t                     seq;
    int                          needs_ownership;

    if (!iface->bulk.available) {
        return 0;
    }

    for (i = 0; i < iface->bulk.window_count; ++i) {
        desc = &iface->bulk.ctrl_descs[i];
        seq  = desc->seq;
        if ((seq == 0) || (desc->sender_generation != iface->bulk.ctrl_generation) ||
            (desc->ack_generation != iface->bulk.ctrl_generation) ||
            (desc->ack_seq != seq)) {
            continue;
        }

        needs_ownership = !!(desc->flags &
                             UCT_OBMM_BULK_DESC_FLAG_REMOTE_OWNERSHIP);
        if (needs_ownership) {
            ucs_memory_bus_load_fence();
            status = uct_obmm_region_set_ownership(iface->bulk.data_region,
                                                   uct_obmm_ep_bulk_window(
                                                           iface->bulk.data_base,
                                                           iface->bulk.window_size,
                                                           i),
                                                   iface->bulk.window_size,
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
        if (iface->bulk.inflight > 0) {
            --iface->bulk.inflight;
        }
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
    for (i = 0; i < iface->bulk.window_count; ++i) {
        index = (iface->bulk.next_window + i) % iface->bulk.window_count;
        desc  = &iface->bulk.ctrl_descs[index];
        if (desc->seq == 0) {
            *window_index_p = index;
            iface->bulk.next_window = (index + 1) % iface->bulk.window_count;
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

    observed_generation = ep->bulk.peer_ctrl->generation;
    publish_seq         = ep->bulk.peer_ctrl->req_seq;
    if (observed_generation != ep->bulk.ctrl_generation) {
        return 0;
    }
    if (publish_seq <= ep->bulk.last_seen_seq) {
        return 0;
    }

    ucs_memory_bus_load_fence();
    for (i = 0; i < ep->bulk.window_count; ++i) {
        desc = &ep->bulk.peer_descs[i];
        if ((desc->seq <= ep->bulk.last_seen_seq) || (desc->seq > publish_seq)) {
            continue;
        }
        if (!uct_obmm_ep_bulk_desc_matches_local_iface(iface, desc)) {
            continue;
        }
        if (desc->cc_memid != ep->bulk.peer_cc_memid) {
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
    if (candidate_desc->length > ep->bulk.window_size) {
        ucs_error("obmm_bulk: invalid bulk length %u for seq=%lu "
                  "(window_size=%zu)",
                  candidate_desc->length, (unsigned long)candidate_desc->seq,
                  ep->bulk.window_size);
        return 0;
    }

    needs_ownership = !!(candidate_desc->flags &
                         UCT_OBMM_BULK_DESC_FLAG_REMOTE_OWNERSHIP);
    window = uct_obmm_ep_bulk_window(ep->bulk.peer_data_base,
                                     ep->bulk.window_size,
                                     candidate_index);
    if (needs_ownership) {
        status = uct_obmm_region_set_ownership(ep->bulk.peer_data_region, window,
                                               ep->bulk.window_size, PROT_READ);
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
    ep->bulk.last_seen_seq = candidate_seq;
    if (needs_ownership) {
        status = uct_obmm_region_set_ownership(ep->bulk.peer_data_region, window,
                                               ep->bulk.window_size, PROT_NONE);
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
uct_obmm_ep_am_short_spsc(uct_obmm_ep_t *ep, uct_obmm_ep_eager_path_t *path,
                          uint8_t id, uint64_t header, const void *payload,
                          unsigned length, size_t payload_total)
{
    uct_obmm_iface_t         *iface = ucs_derived_of(ep->super.super.iface,
                                                    uct_obmm_iface_t);
    uct_obmm_short_lane_t    *lane = path->short_lane.lane;
    uct_obmm_fifo_element_t  *elem;
    uint64_t                  head = path->short_lane.head;

    if (!path->short_lane.active) {
        uct_obmm_ep_short_lane_activate(path->short_lane.active_mask_p,
                                       path->short_lane.lane_index);
        path->short_lane.active = 1;
    }

    if ((head - path->short_lane.cached_tail) >= UCT_OBMM_SHORT_LANE_FIFO_SIZE) {
        ucs_memory_bus_load_fence();
        path->short_lane.cached_tail = lane->ctl.tail;
        if ((head - path->short_lane.cached_tail) >=
            UCT_OBMM_SHORT_LANE_FIFO_SIZE) {
            return UCS_ERR_NO_RESOURCE;
        }
    }

    elem             = uct_obmm_short_lane_elem(lane, head);
    elem->flags      = 0;
    elem->am_id      = id;
    elem->length     = (uint16_t)payload_total;
    elem->generation = path->generation;
    elem->header     = header;
    if (length > 0) {
        memcpy(elem + 1, payload, length);
    }

    ucs_memory_bus_store_fence();
    lane->ctl.head         = head + 1;
    path->short_lane.head  = head + 1;
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
    uct_obmm_region_t            *nc_region;
    uct_obmm_region_t            *cc_region;
    uct_obmm_pool_t               nc_pool;
    uct_obmm_pool_t               cc_pool;
    void                         *peer_slot;
    void                         *cc_pool_base;
    size_t                        cc_pool_length;
    void                         *peer_bulk_data_base;
    size_t                        peer_bulk_data_offset;
    size_t                        peer_bulk_data_length;
    ucs_status_t                  status;
    uct_obmm_region_t            *exp_r;
    uct_obmm_eid_t                nc_eid;
    uct_obmm_eid_t                cc_eid;

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);

    ucs_arbiter_group_init(&self->arb_group);
    ucs_list_head_init(&self->list);
    memset(&self->nc, 0, sizeof(self->nc));
    memset(&self->cc, 0, sizeof(self->cc));
    memset(&self->bulk, 0, sizeof(self->bulk));
    memset(&nc_pool, 0, sizeof(nc_pool));
    memset(&cc_pool, 0, sizeof(cc_pool));

    daddr = (const uct_obmm_device_addr_t*)params->dev_addr;
    iaddr = (const uct_obmm_iface_addr_t*)params->iface_addr;

    if (UCT_OBMM_IFACE_ADDR_GET_VERSION(iaddr->version_flags) !=
        UCT_OBMM_IFACE_ADDR_VERSION) {
        ucs_error("obmm: unsupported peer iface address version %u",
                  (unsigned)UCT_OBMM_IFACE_ADDR_GET_VERSION(iaddr->version_flags));
        return UCS_ERR_UNREACHABLE;
    }
    if (!(UCT_OBMM_IFACE_ADDR_GET_FLAGS(iaddr->version_flags) &
          UCT_OBMM_IFACE_ADDR_FLAG_CC_EAGER) ||
        !(UCT_OBMM_IFACE_ADDR_GET_FLAGS(iaddr->version_flags) &
          UCT_OBMM_IFACE_ADDR_FLAG_BULK)) {
        ucs_error("obmm: peer obmm iface is missing unified CC/bulk support");
        return UCS_ERR_UNREACHABLE;
    }
    if ((iaddr->nc.fifo_size != iface->nc.fifo_size) ||
        (iaddr->nc.fifo_elem_size != iface->nc.fifo_elem_size) ||
        (iaddr->nc.bcopy_seg_size != iface->nc.bcopy_seg_size)) {
        ucs_error("obmm: peer eager geometry differs from local unified iface");
        return UCS_ERR_UNREACHABLE;
    }
    if ((iaddr->bulk_window_size != iface->bulk.window_size) ||
        (iaddr->bulk_window_count != iface->bulk.window_count) ||
        (iaddr->bulk_data_offset != iface->bulk.data_offset) ||
        (iaddr->bulk_cc_memid == 0)) {
        ucs_error("obmm: peer bulk geometry differs from local unified iface");
        return UCS_ERR_UNREACHABLE;
    }

    nc_eid.hi = daddr->exporter_deid_hi;
    nc_eid.lo = daddr->exporter_deid_lo;
    exp_r = uct_obmm_md_export_region_by_mode(md, UCT_OBMM_MAP_MODE_NC);
    if ((exp_r != NULL) &&
        (exp_r->info.exporter_dcna == daddr->exporter_dcna) &&
        (exp_r->info.exporter_deid.hi == nc_eid.hi) &&
        (exp_r->info.exporter_deid.lo == nc_eid.lo)) {
        nc_region = exp_r;
    } else {
        nc_region = uct_obmm_md_find_import_region_by_mode(md, UCT_OBMM_MAP_MODE_NC,
                                                           daddr->exporter_dcna,
                                                           &nc_eid);
    }
    if (nc_region == NULL) {
        ucs_error("obmm: ep_create cannot find peer NC region");
        return UCS_ERR_UNREACHABLE;
    }

    cc_eid.hi = iaddr->cc.exporter_deid_hi;
    cc_eid.lo = iaddr->cc.exporter_deid_lo;
    exp_r = uct_obmm_md_export_region_by_mode(md, UCT_OBMM_MAP_MODE_CC);
    if ((exp_r != NULL) && (exp_r->info.memid == iaddr->bulk_cc_memid) &&
        (exp_r->info.exporter_dcna == iaddr->cc.exporter_dcna) &&
        (exp_r->info.exporter_deid.hi == cc_eid.hi) &&
        (exp_r->info.exporter_deid.lo == cc_eid.lo)) {
        cc_region = exp_r;
    } else {
        cc_region = uct_obmm_md_find_import_region_by_mode_memid(
                md, UCT_OBMM_MAP_MODE_CC, iaddr->cc.exporter_dcna, &cc_eid,
                iaddr->bulk_cc_memid);
    }
    if (cc_region == NULL) {
        ucs_error("obmm: ep_create cannot find peer CC region memid=%lu",
                  (unsigned long)iaddr->bulk_cc_memid);
        return UCS_ERR_UNREACHABLE;
    }

    status = uct_obmm_pool_open(nc_region->base, nc_region->length, &nc_pool);
    if (status != UCS_OK) {
        ucs_error("obmm: failed to open peer NC pool: %s",
                  ucs_status_string(status));
        return status;
    }
    if ((iaddr->nc.slot_index >= nc_pool.slot_count) ||
        (iaddr->bulk_ctrl_slot_index >= nc_pool.slot_count)) {
        ucs_error("obmm: peer NC slot index out of range");
        return UCS_ERR_INVALID_PARAM;
    }
    if (nc_pool.slot_size !=
        uct_obmm_slot_stride(iaddr->nc.fifo_size, iaddr->nc.fifo_elem_size,
                             iaddr->nc.bcopy_seg_size)) {
        ucs_error("obmm: peer NC pool slot_size %u inconsistent with iface_addr "
                  "geometry", nc_pool.slot_size);
        return UCS_ERR_INVALID_PARAM;
    }
    self->peer_nc_dcna = daddr->exporter_dcna;
    self->peer_nc_deid_hi = daddr->exporter_deid_hi;
    self->peer_nc_deid_lo = daddr->exporter_deid_lo;
    self->peer_cc_dcna = iaddr->cc.exporter_dcna;
    self->peer_cc_deid_hi = iaddr->cc.exporter_deid_hi;
    self->peer_cc_deid_lo = iaddr->cc.exporter_deid_lo;
    /* ep_create has already resolved the peer wire identity to concrete mapped
     * regions. Use that result directly instead of re-guessing locality from
     * packed exporter ids. */
    self->is_local = (nc_region == iface->nc_region) &&
                     (cc_region == iface->cc_region);
    if (((iface->role == UCT_OBMM_IFACE_ROLE_CC) && !self->is_local) ||
        ((iface->role == UCT_OBMM_IFACE_ROLE_NC) && self->is_local)) {
        ucs_error("%s: ep_create rejects %s peer",
                  (iface->role == UCT_OBMM_IFACE_ROLE_CC) ? "obmm_cc" :
                                                            "obmm_nc",
                  self->is_local ? "same-node" : "cross-node");
        return UCS_ERR_UNREACHABLE;
    }

    peer_slot = uct_obmm_pool_slot_ptr(&nc_pool, iaddr->nc.slot_index);
    self->nc.available      = 1;
    self->nc.peer_slot      = peer_slot;
    self->nc.peer_ctl       = uct_obmm_slot_ctl(peer_slot);
    self->nc.peer_elems     = uct_obmm_slot_elems(peer_slot);
    self->nc.peer_descs     = uct_obmm_slot_descs(peer_slot, iaddr->nc.fifo_size,
                                                  iaddr->nc.fifo_elem_size);
    self->nc.cached_tail    = self->nc.peer_ctl->tail;
    self->nc.slot_index     = iaddr->nc.slot_index;
    self->nc.generation     = iaddr->nc.generation;
    self->nc.fifo_size      = iaddr->nc.fifo_size;
    self->nc.fifo_mask      = iaddr->nc.fifo_size - 1u;
    self->nc.fifo_elem_size = iaddr->nc.fifo_elem_size;
    self->nc.bcopy_seg_size = iaddr->nc.bcopy_seg_size;
    uct_obmm_ep_init_short_lane(&self->nc, iface->nc.slot_index,
                                iface->nc.generation, peer_slot,
                                self->is_local);

    self->cc.slot_index     = iaddr->cc.slot_index;
    self->cc.generation     = iaddr->cc.generation;
    self->cc.fifo_size      = iface->cc.fifo_size;
    self->cc.fifo_mask      = iface->cc.fifo_mask;
    self->cc.fifo_elem_size = iface->cc.fifo_elem_size;
    self->cc.bcopy_seg_size = iface->cc.bcopy_seg_size;
    if (self->is_local) {
        status = uct_obmm_cc_local_pool_region(cc_region->base, cc_region->length,
                                               &cc_pool_base, &cc_pool_length);
        if (status != UCS_OK) {
            ucs_error("obmm: peer CC region memid=%lu is smaller than the "
                      "computed obmm local prefix",
                      (unsigned long)cc_region->info.memid);
            return status;
        }
        status = uct_obmm_pool_open(cc_pool_base, cc_pool_length, &cc_pool);
        if (status != UCS_OK) {
            ucs_error("obmm: failed to open peer CC pool: %s",
                      ucs_status_string(status));
            return status;
        }
        if (iaddr->cc.slot_index >= cc_pool.slot_count) {
            ucs_error("obmm: peer CC slot_index %u out of range (slot_count=%u)",
                      (unsigned)iaddr->cc.slot_index, cc_pool.slot_count);
            return UCS_ERR_INVALID_PARAM;
        }
        if (cc_pool.slot_size !=
            uct_obmm_slot_stride(UCT_OBMM_CC_LOCAL_FIFO_SIZE,
                                 UCT_OBMM_CC_LOCAL_FIFO_ELEM_SIZE,
                                 UCT_OBMM_CC_LOCAL_BCOPY_SEG_SIZE)) {
            ucs_error("obmm: peer CC pool slot_size %u inconsistent with "
                      "built-in local geometry", cc_pool.slot_size);
            return UCS_ERR_INVALID_PARAM;
        }

        peer_slot = uct_obmm_pool_slot_ptr(&cc_pool, iaddr->cc.slot_index);
        self->cc.available   = 1;
        self->cc.peer_slot   = peer_slot;
        self->cc.peer_ctl    = uct_obmm_slot_ctl(peer_slot);
        self->cc.peer_elems  = uct_obmm_slot_elems(peer_slot);
        self->cc.peer_descs  = uct_obmm_slot_descs(peer_slot,
                                                   UCT_OBMM_CC_LOCAL_FIFO_SIZE,
                                                   UCT_OBMM_CC_LOCAL_FIFO_ELEM_SIZE);
        self->cc.cached_tail = self->cc.peer_ctl->tail;
        uct_obmm_ep_init_short_lane(&self->cc, iface->cc.slot_index,
                                    iface->cc.generation, peer_slot, 1);
    }

    if (uct_obmm_cc_bulk_data_region_split(cc_region->base, cc_region->length,
                                           &peer_bulk_data_base,
                                           &peer_bulk_data_offset,
                                           &peer_bulk_data_length) != UCS_OK) {
        ucs_error("obmm: peer CC bulk region memid=%lu has no space after the "
                  "computed obmm local prefix",
                  (unsigned long)cc_region->info.memid);
        return UCS_ERR_UNREACHABLE;
    }
    if (((size_t)iaddr->bulk_window_count * iaddr->bulk_window_size) >
        peer_bulk_data_length) {
        ucs_error("obmm: peer bulk arena is too small for the advertised "
                  "window geometry (%u * %zu > %zu)",
                  (unsigned)iaddr->bulk_window_count,
                  (size_t)iaddr->bulk_window_size,
                  peer_bulk_data_length);
        return UCS_ERR_UNREACHABLE;
    }
    (void)peer_bulk_data_offset;

    peer_slot = uct_obmm_pool_slot_ptr(&nc_pool, iaddr->bulk_ctrl_slot_index);
    self->bulk.available       = 1;
    self->bulk.peer_slot       = peer_slot;
    self->bulk.ctrl_slot_index = iaddr->bulk_ctrl_slot_index;
    self->bulk.ctrl_generation = iaddr->bulk_ctrl_generation;
    self->bulk.peer_cc_memid   = iaddr->bulk_cc_memid;
    self->bulk.peer_data_region = cc_region;
    self->bulk.peer_data_base  = peer_bulk_data_base;
    self->bulk.peer_ctrl       = uct_obmm_bulk_ctrl_hdr(peer_slot);
    self->bulk.peer_descs      = uct_obmm_bulk_ctrl_descs(peer_slot);
    self->bulk.window_size     = iaddr->bulk_window_size;
    self->bulk.window_count    = iaddr->bulk_window_count;
    self->bulk.last_seen_seq   = 0;
    self->bulk.peer_local      = self->is_local;
    if ((self->bulk.peer_ctrl->magic != UCT_OBMM_BULK_CTRL_MAGIC) ||
        (self->bulk.peer_ctrl->generation != iaddr->bulk_ctrl_generation) ||
        (self->bulk.peer_ctrl->version != UCT_OBMM_BULK_CTRL_VERSION) ||
        (self->bulk.peer_ctrl->cc_memid != iaddr->bulk_cc_memid) ||
        (self->bulk.peer_ctrl->window_size != iaddr->bulk_window_size) ||
        (self->bulk.peer_ctrl->window_count != iaddr->bulk_window_count)) {
        ucs_error("obmm: peer bulk control slot header mismatch");
        return UCS_ERR_UNREACHABLE;
    }

    ucs_list_add_tail(&iface->ep_list, &self->list);
    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_obmm_ep_t)
{
    ucs_list_del(&self->list);
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

    return (UCT_OBMM_IFACE_ADDR_GET_VERSION(iaddr->version_flags) ==
            UCT_OBMM_IFACE_ADDR_VERSION) &&
           (daddr->exporter_dcna == ep->peer_nc_dcna) &&
           (daddr->exporter_deid_hi == ep->peer_nc_deid_hi) &&
           (daddr->exporter_deid_lo == ep->peer_nc_deid_lo) &&
           (iaddr->cc.exporter_dcna == ep->peer_cc_dcna) &&
           (iaddr->cc.exporter_deid_hi == ep->peer_cc_deid_hi) &&
           (iaddr->cc.exporter_deid_lo == ep->peer_cc_deid_lo) &&
           (iaddr->nc.slot_index == ep->nc.slot_index) &&
           (iaddr->nc.generation == ep->nc.generation) &&
           (iaddr->cc.slot_index == ep->cc.slot_index) &&
           (iaddr->cc.generation == ep->cc.generation) &&
           (iaddr->bulk_ctrl_slot_index == ep->bulk.ctrl_slot_index) &&
           (iaddr->bulk_ctrl_generation == ep->bulk.ctrl_generation) &&
           (iaddr->bulk_window_size == ep->bulk.window_size) &&
           (iaddr->bulk_window_count == ep->bulk.window_count) &&
           (iaddr->bulk_cc_memid == ep->bulk.peer_cc_memid);
}


ucs_status_t uct_obmm_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t header,
                                  const void *payload, unsigned length)
{
    uct_obmm_ep_t           *ep    = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    uct_obmm_ep_eager_path_t *path = ep->is_local ? &ep->cc : &ep->nc;
    size_t                   payload_total = sizeof(header) + length;

    UCT_CHECK_AM_ID(id);
    UCT_CHECK_LENGTH(payload_total, 0, uct_obmm_short_lane_max_short(),
                     "am_short");
    return uct_obmm_ep_am_short_spsc(ep, path, id, header, payload, length,
                                     payload_total);
}


/* Reserve one slot in the peer's FIFO, returning the head index that was
 * claimed. Use CAS (not FAA): if an FAA claim succeeds and the FIFO then turns
 * out to be full, the head bump cannot be rolled back and would leave a
 * permanent hole. On aarch64 NC mappings this must be an explicit LSE CAS, not
 * a compiler-default LL/SC atomic. */
static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_ep_reserve_slot(uct_obmm_ep_eager_path_t *path, uct_base_ep_t *ep,
                         uint64_t *head_p)
{
    uint64_t head;

    for (;;) {
        head = path->peer_ctl->head;

        if ((head - path->cached_tail) >= path->fifo_size) {
            ucs_memory_bus_load_fence();
            path->cached_tail = path->peer_ctl->tail;
            if ((head - path->cached_tail) >= path->fifo_size) {
                UCS_STATS_UPDATE_COUNTER(ep->stats, UCT_EP_STAT_NO_RES,
                                         1);
                return UCS_ERR_NO_RESOURCE;
            }
        }

        if (uct_obmm_atomic_bool_cswap64(&path->peer_ctl->head, head,
                                         head + 1)) {
            *head_p = head;
            return UCS_OK;
        }
    }
}


static UCS_F_ALWAYS_INLINE ssize_t
uct_obmm_ep_send_eager_bcopy(uct_obmm_ep_t *ep, uct_obmm_iface_t *iface,
                             uct_obmm_ep_eager_path_t *path, uint8_t id,
                             const void *src, size_t length)
{
    uct_obmm_fifo_element_t *elem;
    void                    *desc;
    uint64_t                 head;
    uint8_t                  owner_bit;
    ucs_status_t             status;

    status = uct_obmm_ep_reserve_slot(path, &ep->super, &head);
    if (status != UCS_OK) {
        return status;
    }
    ucs_assertv(length <= path->bcopy_seg_size,
                "obmm: eager fallback length %zu > bcopy_seg_size=%u",
                length, path->bcopy_seg_size);
    ucs_assertv(length <= UINT16_MAX,
                "obmm: eager fallback length %zu > UINT16_MAX", length);

    elem = uct_obmm_slot_elem(path->peer_elems, head, path->fifo_mask,
                              path->fifo_elem_size);
    desc = uct_obmm_slot_desc(path->peer_descs, head, path->fifo_mask,
                              path->bcopy_seg_size);
    memcpy(desc, src, length);

    elem->am_id      = id;
    elem->length     = (uint16_t)length;
    elem->generation = path->generation;
    elem->header     = 0;
    owner_bit        = (head & path->fifo_size) ? 0u :
                       UCT_OBMM_FIFO_ELEM_FLAG_OWNER;
    ucs_memory_bus_store_fence();
    elem->flags      = owner_bit | UCT_OBMM_FIFO_ELEM_FLAG_BCOPY;

    UCT_TL_EP_STAT_OP(&ep->super, AM, BCOPY, length);
    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, id,
                       desc, length, "TX: AM_BCOPY");
    return (ssize_t)length;
}


static UCS_F_ALWAYS_INLINE ssize_t
uct_obmm_ep_send_local_bcopy(uct_obmm_ep_t *ep, uct_obmm_iface_t *iface,
                             uct_obmm_ep_eager_path_t *path, uint8_t id,
                             uct_pack_callback_t pack_cb, void *arg)
{
    uct_obmm_fifo_element_t *elem;
    void                    *desc;
    uint64_t                 head;
    uint8_t                  owner_bit;
    size_t length;
    ucs_status_t             status;

    ucs_assert(iface->role == UCT_OBMM_IFACE_ROLE_CC);
    status = uct_obmm_ep_reserve_slot(path, &ep->super, &head);
    if (status != UCS_OK) {
        return status;
    }

    elem = uct_obmm_slot_elem(path->peer_elems, head, path->fifo_mask,
                              path->fifo_elem_size);
    desc = uct_obmm_slot_desc(path->peer_descs, head, path->fifo_mask,
                              path->bcopy_seg_size);
    length = pack_cb(desc, arg);
    ucs_assertv(length <= path->bcopy_seg_size,
                "obmm_cc: local am_bcopy length %zu exceeds eager segment %u",
                length, path->bcopy_seg_size);
    ucs_assertv(length <= UINT16_MAX,
                "obmm_cc: local am_bcopy length %zu > UINT16_MAX", length);

    elem->am_id      = id;
    elem->length     = (uint16_t)length;
    elem->generation = path->generation;
    elem->header     = 0;
    owner_bit        = (head & path->fifo_size) ? 0u :
                       UCT_OBMM_FIFO_ELEM_FLAG_OWNER;
    ucs_memory_bus_store_fence();
    elem->flags      = owner_bit | UCT_OBMM_FIFO_ELEM_FLAG_BCOPY;

    UCT_TL_EP_STAT_OP(&ep->super, AM, BCOPY, length);
    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, id,
                       desc, length, "TX: AM_BCOPY_LOCAL");
    return (ssize_t)length;
}


ssize_t uct_obmm_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                             uct_pack_callback_t pack_cb, void *arg,
                             unsigned flags)
{
    uct_obmm_ep_t           *ep    = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    uct_obmm_iface_t        *iface = ucs_derived_of(tl_ep->iface,
                                                    uct_obmm_iface_t);
    uct_obmm_ep_eager_path_t *path = ep->is_local ? &ep->cc : &ep->nc;
    uct_obmm_bulk_window_desc_t *bulk_desc;
    void                    *window;
    uint64_t                 seq;
    size_t                   length;
    ucs_status_t             status;
    unsigned                 window_index;
    int                      use_ownership;

    (void)flags;

    UCT_CHECK_AM_ID(id);

    if (ep->is_local) {
        return uct_obmm_ep_send_local_bcopy(ep, iface, path, id, pack_cb, arg);
    }

    status = uct_obmm_ep_bulk_find_window(iface, &window_index);
    if (status != UCS_OK) {
        return status;
    }

    window = uct_obmm_ep_bulk_window(iface->bulk.data_base,
                                     iface->bulk.window_size, window_index);
    length = pack_cb(window, arg);
    ucs_assertv(length <= iface->bulk.window_size,
                "obmm: pack_cb returned %zu > window_size=%zu",
                length, iface->bulk.window_size);

    if (length <= path->bcopy_seg_size) {
        return uct_obmm_ep_send_eager_bcopy(ep, iface, path, id, window, length);
    }

    use_ownership = !ep->bulk.peer_local;
    if (use_ownership) {
        status = uct_obmm_region_set_ownership(iface->bulk.data_region, window,
                                               iface->bulk.window_size,
                                               PROT_READ);
        if (status != UCS_OK) {
            return status;
        }
    }

    bulk_desc = &iface->bulk.ctrl_descs[window_index];
    seq = iface->bulk.ctrl_hdr->req_seq + 1;
    bulk_desc->ack_seq           = 0;
    bulk_desc->cc_memid          = iface->bulk.data_region->info.memid;
    bulk_desc->length            = (uint32_t)length;
    bulk_desc->target_slot_index = ep->bulk.ctrl_slot_index;
    bulk_desc->target_generation = ep->bulk.ctrl_generation;
    bulk_desc->am_id             = id;
    bulk_desc->flags             = use_ownership ?
                                   UCT_OBMM_BULK_DESC_FLAG_REMOTE_OWNERSHIP : 0;
    bulk_desc->sender_generation = iface->bulk.ctrl_generation;
    bulk_desc->ack_generation    = 0;
    bulk_desc->seq               = seq;
    ucs_memory_bus_store_fence();
    iface->bulk.ctrl_hdr->req_seq = seq;
    ++iface->bulk.inflight;

    UCT_TL_EP_STAT_OP(&ep->super, AM, BCOPY, length);
    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, id,
                       window, length, "TX: AM_BCOPY_BULK");
    return (ssize_t)length;
}


/* Returns true iff the peer's FIFO has at least one free slot, refreshing
 * cached_tail (with a bus_load_fence pair) before declaring "full". Mirrors
 * the resource check used by mm in pending_add. */
static UCS_F_ALWAYS_INLINE int
uct_obmm_ep_has_eager_tx_resource(uct_obmm_ep_eager_path_t *path)
{
    uint64_t head;

    head = path->peer_ctl->head;
    if ((head - path->cached_tail) < path->fifo_size) {
        return 1;
    }
    ucs_memory_bus_load_fence();
    path->cached_tail = path->peer_ctl->tail;
    return (head - path->cached_tail) < path->fifo_size;
}


static UCS_F_ALWAYS_INLINE int
uct_obmm_ep_has_tx_resource(uct_obmm_ep_t *ep)
{
    uct_obmm_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                             uct_obmm_iface_t);
    uct_obmm_ep_eager_path_t *path = ep->is_local ? &ep->cc : &ep->nc;
    unsigned window_index;

    if (!uct_obmm_ep_has_eager_tx_resource(path)) {
        return 0;
    }

    if (ep->is_local) {
        return 1;
    }

    return (uct_obmm_ep_bulk_find_window(iface, &window_index) == UCS_OK);
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
