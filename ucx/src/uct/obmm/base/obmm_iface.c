/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "obmm_iface.h"
#include "obmm_ep.h"
#include "obmm_pool.h"
#include "obmm_fifo.h"
#include "obmm_ownership.h"

#include <uct/api/v2/uct_v2.h>
#include <uct/base/uct_log.h>
#include <ucs/arch/cpu.h>
#include <ucs/debug/log.h>
#include <ucs/sys/math.h>
#include <ucs/sys/sys.h>
#include <ucs/type/class.h>

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>


static uct_iface_ops_t          uct_obmm_iface_ops;
static uct_iface_internal_ops_t uct_obmm_iface_internal_ops;

#define UCT_OBMM_CC_DEVICE_NAME "obmm_cc"
#define UCT_OBMM_NC_DEVICE_NAME "obmm_nc"

static UCS_F_ALWAYS_INLINE const char *
uct_obmm_iface_role_name(uct_obmm_iface_role_t role)
{
    return (role == UCT_OBMM_IFACE_ROLE_CC) ? "obmm_cc" : "obmm_nc";
}


static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_iface_role_from_tl_name(const char *tl_name,
                                 uct_obmm_iface_role_t *role_p)
{
    if (tl_name == NULL) {
        return UCS_ERR_INVALID_PARAM;
    }

    if (!strcmp(tl_name, UCT_OBMM_CC_DEVICE_NAME)) {
        *role_p = UCT_OBMM_IFACE_ROLE_CC;
        return UCS_OK;
    }

    if (!strcmp(tl_name, UCT_OBMM_NC_DEVICE_NAME)) {
        *role_p = UCT_OBMM_IFACE_ROLE_NC;
        return UCS_OK;
    }

    ucs_error("obmm: unsupported TL name '%s'", tl_name);
    return UCS_ERR_INVALID_PARAM;
}


static UCS_F_ALWAYS_INLINE int
uct_obmm_iface_peer_is_local(const uct_obmm_iface_t *iface,
                             const uct_obmm_region_t *nc_region,
                             const uct_obmm_region_t *cc_region)
{
    return (nc_region == iface->nc_region) && (cc_region == iface->cc_region);
}


static ucs_status_t
uct_obmm_iface_query_tl_devices_common(uct_md_h md, const char *dev_name,
                                       uct_tl_device_resource_t **tl_devices_p,
                                       unsigned *num_tl_devices_p)
{
    uct_obmm_md_t *obmm_md = ucs_derived_of(md, uct_obmm_md_t);

    if ((uct_obmm_md_export_region_by_mode(obmm_md, UCT_OBMM_MAP_MODE_NC) == NULL) ||
        (uct_obmm_md_export_region_by_mode(obmm_md, UCT_OBMM_MAP_MODE_CC) == NULL)) {
        return UCS_ERR_NO_DEVICE;
    }

    return uct_single_device_resource(md, dev_name,
                                      UCT_DEVICE_TYPE_SHM,
                                      UCS_SYS_DEVICE_ID_UNKNOWN, tl_devices_p,
                                      num_tl_devices_p);
}


static ucs_status_t
uct_obmm_cc_iface_query_tl_devices(uct_md_h md,
                                   uct_tl_device_resource_t **tl_devices_p,
                                   unsigned *num_tl_devices_p)
{
    return uct_obmm_iface_query_tl_devices_common(md, UCT_OBMM_CC_DEVICE_NAME,
                                                  tl_devices_p,
                                                  num_tl_devices_p);
}


static ucs_status_t
uct_obmm_nc_iface_query_tl_devices(uct_md_h md,
                                   uct_tl_device_resource_t **tl_devices_p,
                                   unsigned *num_tl_devices_p)
{
    return uct_obmm_iface_query_tl_devices_common(md, UCT_OBMM_NC_DEVICE_NAME,
                                                  tl_devices_p,
                                                  num_tl_devices_p);
}


static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_iface_cc_pool_region(uct_obmm_region_t *region, void **base_p,
                              size_t *length_p)
{
    size_t       prefix_size;
    ucs_status_t status;

    status = uct_obmm_cc_local_layout(NULL, NULL, &prefix_size);
    if (status != UCS_OK) {
        ucs_error("obmm: failed to compute obmm CC-local prefix size");
        return status;
    }
    if (uct_obmm_cc_local_pool_region(region->base, region->length, base_p,
                                      length_p) != UCS_OK) {
        ucs_error("obmm: CC region memid=%lu is smaller than the computed "
                  "obmm local prefix (%zu < %zu)",
                  (unsigned long)region->info.memid, region->length, prefix_size);
        return UCS_ERR_NO_RESOURCE;
    }

    return UCS_OK;
}


static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_iface_bulk_data_region(uct_obmm_region_t *region, void **base_p,
                                size_t *offset_p, size_t *length_p)
{
    size_t       prefix_size;
    ucs_status_t status;

    if (uct_obmm_cc_bulk_data_region_split(region->base, region->length,
                                           base_p, offset_p,
                                           length_p) != UCS_OK) {
        status = uct_obmm_cc_local_layout(NULL, NULL, &prefix_size);
        if (status != UCS_OK) {
            ucs_error("obmm: failed to compute obmm_cc local prefix size");
            return status;
        }
        ucs_error("obmm: CC region memid=%lu has no space left for cc_bulk "
                  "after the computed obmm_cc local prefix (%zu bytes)",
                  (unsigned long)region->info.memid, prefix_size);
        return UCS_ERR_NO_RESOURCE;
    }
    return UCS_OK;
}


static UCS_F_ALWAYS_INLINE void *
uct_obmm_iface_bulk_window(uct_obmm_iface_t *iface, unsigned window_index)
{
    return UCS_PTR_BYTE_OFFSET(iface->bulk.data_base,
                               (size_t)window_index * iface->bulk.window_size);
}


static void uct_obmm_iface_bulk_cleanup_windows(uct_obmm_iface_t *iface)
{
    ucs_status_t status;
    unsigned     i;

    if (!iface->bulk.available) {
        return;
    }

    uct_obmm_iface_bulk_reclaim_windows(iface);
    for (i = 0; i < iface->bulk.window_count; ++i) {
        status = uct_obmm_region_set_ownership(iface->bulk.data_region,
                                               uct_obmm_iface_bulk_window(iface, i),
                                               iface->bulk.window_size,
                                               PROT_WRITE);
        if (status != UCS_OK) {
            ucs_warn("obmm_bulk: failed to restore window %u to PROT_WRITE "
                     "during iface cleanup", i);
            continue;
        }

        iface->bulk.ctrl_descs[i].ack_seq           = 0;
        iface->bulk.ctrl_descs[i].cc_memid          = 0;
        iface->bulk.ctrl_descs[i].length            = 0;
        iface->bulk.ctrl_descs[i].target_slot_index = 0;
        iface->bulk.ctrl_descs[i].target_generation = 0;
        iface->bulk.ctrl_descs[i].am_id             = 0;
        iface->bulk.ctrl_descs[i].flags             = 0;
        iface->bulk.ctrl_descs[i].sender_generation = 0;
        iface->bulk.ctrl_descs[i].ack_generation    = 0;
        iface->bulk.ctrl_descs[i].seq               = 0;
    }

    iface->bulk.ctrl_hdr->req_seq = 0;
    ucs_memory_bus_store_fence();
}


static UCS_F_ALWAYS_INLINE void
uct_obmm_iface_fifo_window_adjust(uct_obmm_iface_t *iface, unsigned rx_count)
{
    if (rx_count < iface->fifo_poll_count) {
        iface->fifo_poll_count = ucs_max(iface->fifo_poll_count /
                                         UCT_OBMM_IFACE_FIFO_MD_FACTOR,
                                         iface->fifo_min_poll);
        iface->fifo_prev_wnd_cons = 0;
        return;
    }

    ucs_assert(rx_count == iface->fifo_poll_count);
    if (iface->fifo_prev_wnd_cons) {
        iface->fifo_poll_count = ucs_min(iface->fifo_poll_count +
                                         UCT_OBMM_IFACE_FIFO_AI_VALUE,
                                         iface->fifo_max_poll);
    } else {
        iface->fifo_prev_wnd_cons = 1;
    }
}


static UCS_F_ALWAYS_INLINE unsigned
uct_obmm_iface_progress_regular_short_lane(uct_obmm_iface_t *iface,
                                           uct_obmm_iface_eager_path_t *path,
                                           unsigned lane_index,
                                           unsigned max_poll,
                                           int *lane_reset_p)
{
    unsigned                  polled = 0;
    uct_obmm_short_lane_t    *lane;
    uct_obmm_fifo_element_t  *elem;
    uint64_t                  head;
    uint64_t                  tail;
    uint64_t                  published_tail;

    *lane_reset_p = 0;
    lane = &path->recv_short_lanes[lane_index];
    tail = path->recv_short_tails[lane_index];
    published_tail = path->recv_short_published_tails[lane_index];
    head = lane->ctl.head;
    if (ucs_unlikely(head < tail)) {
        *lane_reset_p = 1;
        tail = lane->ctl.tail;
        path->recv_short_tails[lane_index] = tail;
        path->recv_short_published_tails[lane_index] = tail;
        published_tail = tail;
    }
    if (tail == head) {
        return 0;
    }

    ucs_memory_bus_load_fence();
    while ((tail != head) && (polled < max_poll)) {
        elem = uct_obmm_short_lane_elem(lane, tail);
        if (elem->generation != path->generation) {
            /* Stale write from a previous slot owner; drop silently. */
        } else if ((elem->length < sizeof(elem->header)) ||
                   (elem->length > uct_obmm_short_lane_max_short())) {
            ucs_error("obmm: invalid short-lane length %u at lane=%u "
                      "tail=%lu elem_gen=%u expected=%u", elem->length,
                      lane_index, (unsigned long)tail, elem->generation,
                      path->generation);
        } else {
            memcpy(iface->short_copy_buf, &elem->header, elem->length);
            uct_iface_invoke_am(&iface->super, elem->am_id,
                                iface->short_copy_buf, elem->length, 0);
        }

        ++tail;
        ++polled;
    }

    path->recv_short_tails[lane_index] = tail;
    if ((tail != published_tail) &&
        ((tail - published_tail) >= UCT_OBMM_SHORT_LANE_TAIL_BATCH)) {
        uct_obmm_bus_full_fence();
        lane->ctl.tail = tail;
        path->recv_short_published_tails[lane_index] = tail;
    }

    return polled;
}


static unsigned
uct_obmm_iface_progress_regular_short_lanes(uct_obmm_iface_t *iface,
                                            uct_obmm_iface_eager_path_t *path,
                                            unsigned max_poll)
{
    uint64_t                  active_mask;
    unsigned                  polled = 0;
    unsigned                  lane_index;
    int                       lane_reset;

    if (path->recv_short_hot_lane < UCT_OBMM_SHORT_LANE_COUNT) {
        polled = uct_obmm_iface_progress_regular_short_lane(
                iface, path, path->recv_short_hot_lane, max_poll, &lane_reset);
        if (lane_reset) {
            path->recv_short_hot_lane = UCT_OBMM_SHORT_LANE_COUNT;
        }
        if (polled > 0) {
            return polled;
        }
    }

    active_mask = *path->recv_short_active_mask;
    ucs_for_each_bit(lane_index, active_mask) {
        if (lane_index == path->recv_short_hot_lane) {
            continue;
        }
        polled += uct_obmm_iface_progress_regular_short_lane(
                iface, path, lane_index, max_poll - polled, &lane_reset);
        if (polled > 0) {
            path->recv_short_hot_lane = lane_index;
        }
        if (polled >= max_poll) {
            break;
        }
    }

    return polled;
}


static unsigned
uct_obmm_iface_progress_short_lanes(uct_obmm_iface_t *iface,
                                    uct_obmm_iface_eager_path_t *path,
                                    unsigned max_poll)
{
    return uct_obmm_iface_progress_regular_short_lanes(iface, path, max_poll);
}


ucs_config_field_t uct_obmm_iface_config_table[] = {
    {"", "", NULL, ucs_offsetof(uct_obmm_iface_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_iface_config_table)},

    {"BW", "3400MBs",
     "Effective transport bandwidth used for UCP lane/protocol cost "
     "modeling. This is not a required knob: if the user does not set "
     "UCX_OBMM_BW, obmm uses this sustained default.",
     ucs_offsetof(uct_obmm_iface_config_t, super.bandwidth), UCS_CONFIG_TYPE_BW},

    {"FIFO_SIZE", "64",
     "Number of elements in the per-iface receive FIFO ring (power of 2).",
     ucs_offsetof(uct_obmm_iface_config_t, fifo_size), UCS_CONFIG_TYPE_UINT},

    {"FIFO_ELEM_SIZE", "64",
     "Size in bytes of a single legacy FIFO element. This no longer controls "
     "am_short capacity: dedicated SPSC short lanes carry inline short data, "
     "while the shared FIFO carries bcopy metadata only. Keep this stride "
     "compact and 64-byte aligned unless measurements justify a larger "
     "metadata footprint.",
        ucs_offsetof(uct_obmm_iface_config_t, fifo_elem_size),
        UCS_CONFIG_TYPE_UINT},

    {"BCOPY_SEG_SIZE", UCT_OBMM_DEFAULT_BCOPY_SEG_SIZE_STR,
     "Size in bytes of each per-FIFO-elem bcopy descriptor. This is "
     "advertised as max_bcopy. Defaults keep raw UCT bcopy at 32KiB for "
     "common medium-message eager traffic, while preserving 64-byte alignment for every "
     "descriptor stride. Larger values reduce UCP fragmentation for medium "
     "messages but may also delay higher-level protocol transitions, so they "
     "are not always faster despite consuming more of the mapped region "
     "(per-slot legacy FIFO footprint = FIFO_SIZE * (FIFO_ELEM_SIZE + "
     "BCOPY_SEG_SIZE)). Capped at 65535 (elem->length is uint16).",
     ucs_offsetof(uct_obmm_iface_config_t, bcopy_seg_size),
     UCS_CONFIG_TYPE_UINT},

    {"WINDOW_SIZE", "2m",
     "obmm_bulk only: bytes per CC ownership epoch/window. Must stay "
     "2 MiB aligned in the current staged design.",
     ucs_offsetof(uct_obmm_iface_config_t, bulk_window_size),
     UCS_CONFIG_TYPE_MEMUNITS},

    {"WINDOW_COUNT", "8",
     "obmm_bulk only: how many CC bulk windows are shared by one sender "
     "iface across all remote peers.",
     ucs_offsetof(uct_obmm_iface_config_t, bulk_window_count),
     UCS_CONFIG_TYPE_UINT},

    {"FIFO_MIN_POLL", "16",
     "Minimal receive completions to drain in one progress() call. Defaults "
     "match the pre-adaptive fixed poll budget for latency-sensitive runs.",
     ucs_offsetof(uct_obmm_iface_config_t, fifo_min_poll),
      UCS_CONFIG_TYPE_ULUNITS},

    {"FIFO_MAX_POLL", "16",
     "Maximal receive completions to drain in one progress() call. Set above "
     "FIFO_MIN_POLL to re-enable adaptive receive polling for throughput "
     "experiments.",
     ucs_offsetof(uct_obmm_iface_config_t, fifo_max_poll),
        UCS_CONFIG_TYPE_ULUNITS},

    {"PENDING_QUOTA", "1",
     "How many pending send retries may be dispatched during iface progress. "
     "Defaults to the latency-friendly single-dispatch behavior.",
     ucs_offsetof(uct_obmm_iface_config_t, pending_quota),
     UCS_CONFIG_TYPE_UINT},

     {NULL}
};


static ucs_status_t uct_obmm_iface_query(uct_iface_h tl_iface,
                                        uct_iface_attr_t *attr)
{
    uct_obmm_iface_t *iface = ucs_derived_of(tl_iface, uct_obmm_iface_t);

    uct_base_iface_query(&iface->super, attr);
    attr->cap.flags              = UCT_IFACE_FLAG_PENDING          |
                                   UCT_IFACE_FLAG_CONNECT_TO_IFACE |
                                   UCT_IFACE_FLAG_CB_SYNC;
    attr->cap.flags             |= UCT_IFACE_FLAG_AM_SHORT |
                                   UCT_IFACE_FLAG_AM_BCOPY;
    if (iface->role == UCT_OBMM_IFACE_ROLE_NC) {
        attr->cap.flags         |= UCT_IFACE_FLAG_INTER_NODE;
    }
    attr->iface_addr_len         = sizeof(uct_obmm_iface_addr_t);
    attr->device_addr_len        = sizeof(uct_obmm_device_addr_t);
    attr->ep_addr_len            = 0;
    attr->max_conn_priv          = 0;

    attr->cap.am.max_short       = uct_obmm_short_lane_max_short();
    attr->cap.am.max_bcopy       = (iface->role == UCT_OBMM_IFACE_ROLE_CC) ?
                                   iface->cc.bcopy_seg_size :
                                   iface->bulk.window_size;
    attr->cap.am.min_zcopy       = 0;
    attr->cap.am.max_zcopy       = 0;
    attr->cap.am.max_iov         = 0;

    attr->cap.put.max_short      = 0;
    attr->cap.put.max_bcopy      = 0;
    attr->cap.put.min_zcopy      = 0;
    attr->cap.put.max_zcopy      = 0;
    attr->cap.put.max_iov        = 0;

    attr->cap.get.max_bcopy      = 0;
    attr->cap.get.min_zcopy      = 0;
    attr->cap.get.max_zcopy      = 0;
    attr->cap.get.max_iov        = 0;

    attr->latency                = UCS_LINEAR_FUNC_ZERO;
    attr->bandwidth.dedicated    = iface->config.bandwidth;
    attr->bandwidth.shared       = 0;
    attr->overhead               = (iface->role == UCT_OBMM_IFACE_ROLE_CC) ?
                                   30e-9 : 100e-9;
    attr->priority               = 1;
    return UCS_OK;
}


static ucs_status_t
uct_obmm_iface_get_device_address(uct_iface_h tl_iface,
                                  uct_device_addr_t *addr)
{
    uct_obmm_iface_t       *iface = ucs_derived_of(tl_iface, uct_obmm_iface_t);
    uct_obmm_device_addr_t *daddr = (uct_obmm_device_addr_t*)addr;

    daddr->exporter_dcna    = iface->nc_region->info.exporter_dcna;
    daddr->exporter_deid_hi = iface->nc_region->info.exporter_deid.hi;
    daddr->exporter_deid_lo = iface->nc_region->info.exporter_deid.lo;
    return UCS_OK;
}


static ucs_status_t uct_obmm_iface_get_address(uct_iface_h tl_iface,
                                               uct_iface_addr_t *addr)
{
    uct_obmm_iface_t      *iface = ucs_derived_of(tl_iface, uct_obmm_iface_t);
    uct_obmm_iface_addr_t *iaddr = (uct_obmm_iface_addr_t*)addr;

    iaddr->version_flags        = UCT_OBMM_IFACE_ADDR_PACK_VERSION_FLAGS(
                                          UCT_OBMM_IFACE_ADDR_VERSION,
                                          UCT_OBMM_IFACE_ADDR_FLAG_CC_EAGER |
                                          UCT_OBMM_IFACE_ADDR_FLAG_BULK);
    iaddr->nc.generation        = iface->nc.generation;
    iaddr->nc.fifo_size         = (uint16_t)iface->nc.fifo_size;
    iaddr->nc.fifo_elem_size    = (uint16_t)iface->nc.fifo_elem_size;
    iaddr->nc.bcopy_seg_size    = (uint16_t)iface->nc.bcopy_seg_size;
    iaddr->nc.slot_index        = (uint8_t)iface->nc.slot_index;
    iaddr->cc.exporter_dcna     = iface->cc_region->info.exporter_dcna;
    iaddr->cc.exporter_deid_hi  = iface->cc_region->info.exporter_deid.hi;
    iaddr->cc.exporter_deid_lo  = iface->cc_region->info.exporter_deid.lo;
    iaddr->cc.generation        = iface->cc.generation;
    iaddr->cc.slot_index        = (uint8_t)iface->cc.slot_index;
    iaddr->bulk_ctrl_generation = iface->bulk.ctrl_generation;
    iaddr->bulk_data_offset     = (uint32_t)iface->bulk.data_offset;
    iaddr->bulk_window_size     = (uint32_t)iface->bulk.window_size;
    iaddr->bulk_cc_memid        = iface->bulk.data_region->info.memid;
    iaddr->bulk_ctrl_slot_index = (uint8_t)iface->bulk.ctrl_slot_index;
    iaddr->bulk_window_count    = (uint8_t)iface->bulk.window_count;
    return UCS_OK;
}


static int
uct_obmm_iface_is_reachable_v2(const uct_iface_h tl_iface,
                               const uct_iface_is_reachable_params_t *params)
{
    uct_obmm_iface_t             *iface = ucs_derived_of(tl_iface,
                                                         uct_obmm_iface_t);
    uct_obmm_md_t                *md    = ucs_derived_of(iface->super.md,
                                                         uct_obmm_md_t);
    const uct_obmm_device_addr_t *daddr;
    const uct_obmm_iface_addr_t  *iaddr;
    uct_obmm_eid_t                nc_eid;
    uct_obmm_eid_t                cc_eid;
    uct_obmm_region_t            *region;
    uint8_t                       version;
    uint8_t                       flags;
    int                           nc_local;
    int                           cc_local;
    int                           peer_local;

    if (!uct_iface_is_reachable_params_addrs_valid(params)) {
        return 0;
    }

    daddr = (const uct_obmm_device_addr_t*)params->device_addr;
    iaddr = (const uct_obmm_iface_addr_t*)params->iface_addr;
    if ((daddr == NULL) || (iaddr == NULL)) {
        uct_iface_fill_info_str_buf(params, "missing device or iface address");
        return 0;
    }

    version = UCT_OBMM_IFACE_ADDR_GET_VERSION(iaddr->version_flags);
    flags   = UCT_OBMM_IFACE_ADDR_GET_FLAGS(iaddr->version_flags);

    if (version != UCT_OBMM_IFACE_ADDR_VERSION) {
        uct_iface_fill_info_str_buf(params,
                                    "unsupported obmm iface address version %u",
                                    (unsigned)version);
        return 0;
    }
    if (!(flags & UCT_OBMM_IFACE_ADDR_FLAG_CC_EAGER) ||
        !(flags & UCT_OBMM_IFACE_ADDR_FLAG_BULK)) {
        uct_iface_fill_info_str_buf(params,
                                    "peer obmm iface is missing unified CC/bulk flags");
        return 0;
    }
    if ((iaddr->nc.fifo_size != iface->nc.fifo_size) ||
        (iaddr->nc.fifo_elem_size != iface->nc.fifo_elem_size) ||
        (iaddr->nc.bcopy_seg_size != iface->nc.bcopy_seg_size)) {
        uct_iface_fill_info_str_buf(params,
                                    "incompatible obmm eager geometry");
        return 0;
    }
    if ((iaddr->bulk_window_size != iface->bulk.window_size) ||
        (iaddr->bulk_window_count != iface->bulk.window_count) ||
        (iaddr->bulk_data_offset != iface->bulk.data_offset) ||
        (iaddr->bulk_cc_memid == 0)) {
        uct_iface_fill_info_str_buf(params,
                                    "incompatible obmm bulk geometry/layout");
        return 0;
    }

    nc_eid.hi = daddr->exporter_deid_hi;
    nc_eid.lo = daddr->exporter_deid_lo;
    region = uct_obmm_md_export_region_by_mode(md, UCT_OBMM_MAP_MODE_NC);
    nc_local = (region != NULL) &&
               (region->info.exporter_dcna == daddr->exporter_dcna) &&
               (region->info.exporter_deid.hi == nc_eid.hi) &&
               (region->info.exporter_deid.lo == nc_eid.lo);
    if (!nc_local &&
        (uct_obmm_md_find_import_region_by_mode(md, UCT_OBMM_MAP_MODE_NC,
                                                daddr->exporter_dcna,
                                                &nc_eid) == NULL)) {
        uct_iface_fill_info_str_buf(params,
                                    "no NC mapped region for peer dcna=0x%lx "
                                    "deid=0x%lx:0x%lx",
                                    (unsigned long)daddr->exporter_dcna,
                                    (unsigned long)nc_eid.hi,
                                    (unsigned long)nc_eid.lo);
        return 0;
    }

    cc_eid.hi = iaddr->cc.exporter_deid_hi;
    cc_eid.lo = iaddr->cc.exporter_deid_lo;
    region = uct_obmm_md_export_region_by_mode(md, UCT_OBMM_MAP_MODE_CC);
    cc_local = (region != NULL) &&
               (region->info.memid == iaddr->bulk_cc_memid) &&
               (region->info.exporter_dcna == iaddr->cc.exporter_dcna) &&
               (region->info.exporter_deid.hi == cc_eid.hi) &&
               (region->info.exporter_deid.lo == cc_eid.lo);
    if (!cc_local &&
        (uct_obmm_md_find_import_region_by_mode_memid(md, UCT_OBMM_MAP_MODE_CC,
                                                      iaddr->cc.exporter_dcna,
                                                      &cc_eid,
                                                      iaddr->bulk_cc_memid) == NULL)) {
        uct_iface_fill_info_str_buf(params,
                                    "no CC mapped region for peer dcna=0x%lx "
                                    "deid=0x%lx:0x%lx memid=%lu",
                                    (unsigned long)iaddr->cc.exporter_dcna,
                                    (unsigned long)cc_eid.hi,
                                    (unsigned long)cc_eid.lo,
                                    (unsigned long)iaddr->bulk_cc_memid);
        return 0;
    }

    peer_local = nc_local && cc_local;
    if (((iface->role == UCT_OBMM_IFACE_ROLE_CC) && !peer_local) ||
        ((iface->role == UCT_OBMM_IFACE_ROLE_NC) && peer_local)) {
        uct_iface_fill_info_str_buf(params,
                                    "%s rejects %s peer",
                                    uct_obmm_iface_role_name(iface->role),
                                    peer_local ? "same-node" : "cross-node");
        return 0;
    }

    return uct_iface_scope_is_reachable(tl_iface, params);
}


static void
uct_obmm_iface_maybe_publish_eager_tail(uct_obmm_iface_t *iface,
                                       uct_obmm_iface_eager_path_t *path)
{
    uint64_t unreleased;

    if (path->read_index == path->recv_published_tail) {
        return;
    }

    unreleased = path->read_index - path->recv_published_tail;
    if ((unreleased < path->recv_tail_batch) &&
        (iface->role != UCT_OBMM_IFACE_ROLE_CC)) {
        return;
    }

    ucs_memory_bus_load_fence();
    if ((path->recv_ctl->head != path->read_index) &&
        (unreleased < path->recv_tail_batch)) {
        return;
    }

    uct_obmm_bus_full_fence();
    path->recv_ctl->tail      = path->read_index;
    path->recv_published_tail = path->read_index;
}


static unsigned
uct_obmm_iface_progress_eager_path(uct_obmm_iface_t *iface,
                                   uct_obmm_iface_eager_path_t *path,
                                   unsigned max_poll)
{
    unsigned                 polled = 0;
    uct_obmm_fifo_element_t *elem;
    uint8_t                  flags;
    uint8_t                  expected_owner;
    int                      regular_first = 0;

    if (!path->available) {
        return 0;
    }

    if (iface->role == UCT_OBMM_IFACE_ROLE_CC) {
        elem = uct_obmm_slot_elem(path->recv_elems, path->read_index,
                                  path->fifo_mask, path->fifo_elem_size);
        expected_owner = (path->read_index & path->fifo_size) ? 0u :
                         UCT_OBMM_FIFO_ELEM_FLAG_OWNER;
        if ((elem->flags & UCT_OBMM_FIFO_ELEM_FLAG_OWNER) == expected_owner) {
            regular_first = 1;
        }
    }

    if (!regular_first) {
        polled = uct_obmm_iface_progress_short_lanes(iface, path, max_poll);
        if ((polled > 0) && ucs_arbiter_is_empty(&iface->arbiter)) {
            ucs_memory_bus_load_fence();
            if (path->recv_ctl->head == path->read_index) {
                uct_obmm_iface_maybe_publish_eager_tail(iface, path);
                uct_obmm_iface_fifo_window_adjust(iface, polled);
                return polled;
            }
        }
    }

    while (polled < max_poll) {
        elem = uct_obmm_slot_elem(path->recv_elems, path->read_index,
                                  path->fifo_mask, path->fifo_elem_size);

        expected_owner = (path->read_index & path->fifo_size) ? 0u :
                         UCT_OBMM_FIFO_ELEM_FLAG_OWNER;

        flags = elem->flags;
        if ((flags & UCT_OBMM_FIFO_ELEM_FLAG_OWNER) != expected_owner) {
            break;
        }

        ucs_memory_bus_load_fence();

        if (elem->generation != path->generation) {
            ucs_trace_data("obmm: drop stale elem (gen=%u expected=%u) at idx=%lu",
                           elem->generation, path->generation,
                           (unsigned long)path->read_index);
        } else if (flags & UCT_OBMM_FIFO_ELEM_FLAG_BCOPY) {
            if (ucs_unlikely(elem->length > path->bcopy_seg_size)) {
                ucs_error("obmm: invalid bcopy length %u at idx=%lu "
                          "(seg_size=%u gen=%u expected=%u)", elem->length,
                          (unsigned long)path->read_index,
                          path->bcopy_seg_size, elem->generation,
                          path->generation);
            } else {
                void *desc = uct_obmm_slot_desc(path->recv_descs,
                                                path->read_index,
                                                path->fifo_mask,
                                                path->bcopy_seg_size);
                uct_iface_invoke_am(&iface->super, elem->am_id,
                                    desc, elem->length, 0);
            }
        } else {
            ucs_error("obmm: unexpected legacy fifo am_short at idx=%lu "
                      "(gen=%u expected=%u); current wire format routes "
                      "am_short through SPSC short lanes only",
                      (unsigned long)path->read_index, elem->generation,
                      path->generation);
        }

        path->read_index++;
        polled++;
    }

    if (regular_first && (polled < max_poll)) {
        polled += uct_obmm_iface_progress_short_lanes(iface, path,
                                                      max_poll - polled);
    }

    uct_obmm_iface_fifo_window_adjust(iface, polled);

    uct_obmm_iface_maybe_publish_eager_tail(iface, path);

    return polled;
}


static unsigned uct_obmm_iface_progress(uct_iface_h tl_iface)
{
    uct_obmm_iface_t        *iface = ucs_derived_of(tl_iface, uct_obmm_iface_t);
    uct_obmm_iface_eager_path_t *path;
    unsigned                 polled = 0;
    unsigned                 pending_progress = 0;
    uct_obmm_ep_t           *ep;
    size_t                   max_poll = iface->fifo_poll_count;

    if (ucs_list_is_empty(&iface->ep_list) &&
        ucs_arbiter_is_empty(&iface->arbiter) &&
        (iface->bulk.inflight == 0)) {
        return 0;
    }

    path = (iface->role == UCT_OBMM_IFACE_ROLE_CC) ? &iface->cc : &iface->nc;
    polled += uct_obmm_iface_progress_eager_path(iface, path, max_poll);
    if (polled > 0) {
        iface->bulk.idle_polls = 0;
        if (!ucs_arbiter_is_empty(&iface->arbiter)) {
            ucs_arbiter_dispatch(&iface->arbiter, 1,
                                 uct_obmm_ep_process_pending,
                                 &pending_progress);
        }
        return polled + pending_progress;
    }

    if ((iface->role == UCT_OBMM_IFACE_ROLE_NC) && iface->bulk.available &&
        (polled < max_poll)) {
        if ((iface->bulk.inflight > 0) ||
            (++iface->bulk.idle_polls >= iface->fifo_min_poll)) {
            iface->bulk.idle_polls = 0;
            polled += uct_obmm_iface_bulk_reclaim_windows(iface);

            ucs_list_for_each(ep, &iface->ep_list, list) {
                if (polled >= max_poll) {
                    break;
                }

                polled += uct_obmm_ep_progress_bulk_rx(ep, iface,
                                                       max_poll - polled);
            }
        }

        if (!ucs_arbiter_is_empty(&iface->arbiter)) {
            ucs_arbiter_dispatch(&iface->arbiter, 1,
                                 uct_obmm_ep_process_pending,
                                 &pending_progress);
        }
        return polled + pending_progress;
    }

    /* Drain any UCP requests waiting on TX backpressure. The peer-side
     * tail advance we just published may also have freed slots that *our*
     * pending eps have been waiting for; dispatch with a fresh head/tail
     * snapshot so queued retries see the latest state. */
    if (!ucs_arbiter_is_empty(&iface->arbiter)) {
        ucs_arbiter_dispatch(&iface->arbiter, 1, uct_obmm_ep_process_pending,
                             &pending_progress);
    }

    return polled + pending_progress;
}


static ucs_status_t uct_obmm_iface_fence(uct_iface_h tl_iface, unsigned flags)
{
    (void)flags;
    ucs_memory_cpu_fence();
    UCT_TL_IFACE_STAT_FENCE(ucs_derived_of(tl_iface, uct_base_iface_t));
    return UCS_OK;
}


static ucs_status_t uct_obmm_ep_fence(uct_ep_h tl_ep, unsigned flags)
{
    (void)flags;
    ucs_memory_cpu_fence();
    UCT_TL_EP_STAT_FENCE(ucs_derived_of(tl_ep, uct_base_ep_t));
    return UCS_OK;
}


static UCS_CLASS_INIT_FUNC(uct_obmm_iface_t, uct_md_h tl_md, uct_worker_h worker,
                           const uct_iface_params_t *params,
                           const uct_iface_config_t *tl_config)
{
    uct_obmm_iface_config_t *config = ucs_derived_of(tl_config,
                                                     uct_obmm_iface_config_t);
    uct_obmm_md_t           *md     = ucs_derived_of(tl_md, uct_obmm_md_t);
    uct_obmm_iface_role_t    role;
    void                    *nc_pool_base;
    size_t                   nc_stride;
    size_t                   nc_required;
    size_t                   nc_pool_length;
    void                    *cc_pool_base;
    size_t                   cc_stride;
    size_t                   cc_required;
    size_t                   cc_pool_length;
    size_t                   ctrl_required;
    void                    *bulk_data_base   = NULL;
    size_t                   bulk_data_length = 0;
    size_t                   bulk_data_offset = 0;
    ucs_status_t             status;

    UCT_CHECK_PARAM(params->field_mask & UCT_IFACE_PARAM_FIELD_OPEN_MODE,
                    "UCT_IFACE_PARAM_FIELD_OPEN_MODE is not defined");
    UCT_CHECK_PARAM(params->field_mask & UCT_IFACE_PARAM_FIELD_DEVICE,
                    "UCT_IFACE_PARAM_FIELD_DEVICE is not defined");
    if (!(params->open_mode & UCT_IFACE_OPEN_MODE_DEVICE)) {
        ucs_error("only UCT_IFACE_OPEN_MODE_DEVICE is supported");
        return UCS_ERR_UNSUPPORTED;
    }
    status = uct_obmm_iface_role_from_tl_name(params->mode.device.tl_name, &role);
    if (status != UCS_OK) {
        return status;
    }

    if (config->fifo_min_poll == 0) {
        ucs_error("obmm: FIFO_MIN_POLL must be > 0");
        return UCS_ERR_INVALID_PARAM;
    }
    if (config->fifo_max_poll < config->fifo_min_poll) {
        ucs_error("obmm: FIFO_MAX_POLL (%zu) must be >= FIFO_MIN_POLL (%zu)",
                  config->fifo_max_poll, config->fifo_min_poll);
        return UCS_ERR_INVALID_PARAM;
    }
    if (config->pending_quota == 0) {
        ucs_error("obmm: PENDING_QUOTA must be > 0");
        return UCS_ERR_INVALID_PARAM;
    }
    if (config->fifo_size == 0) {
        ucs_error("obmm: FIFO_SIZE must be > 0");
        return UCS_ERR_INVALID_PARAM;
    }
    if (!ucs_is_pow2(config->fifo_size)) {
        ucs_error("obmm: FIFO_SIZE (%u) must be a power of 2",
                  config->fifo_size);
        return UCS_ERR_INVALID_PARAM;
    }
    if (config->fifo_elem_size <= sizeof(uct_obmm_fifo_element_t)) {
        ucs_error("obmm: FIFO_ELEM_SIZE (%u) must be > %zu",
                  config->fifo_elem_size, sizeof(uct_obmm_fifo_element_t));
        return UCS_ERR_INVALID_PARAM;
    }
    if (config->bcopy_seg_size == 0) {
        ucs_error("obmm: BCOPY_SEG_SIZE must be > 0");
        return UCS_ERR_INVALID_PARAM;
    }
    if (config->bcopy_seg_size > UINT16_MAX) {
        ucs_error("obmm: BCOPY_SEG_SIZE (%u) too large; max_bcopy must fit in "
                  "uint16 (max %u)",
                  config->bcopy_seg_size, (unsigned)UINT16_MAX);
        return UCS_ERR_INVALID_PARAM;
    }
    if (config->bulk_window_size == 0) {
        ucs_error("obmm: WINDOW_SIZE must be > 0");
        return UCS_ERR_INVALID_PARAM;
    }
    if ((config->bulk_window_size % UCT_OBMM_CC_BULK_WINDOW_ALIGN) != 0) {
        ucs_error("obmm: WINDOW_SIZE (%zu) must be %zu-byte aligned",
                  config->bulk_window_size,
                  (size_t)UCT_OBMM_CC_BULK_WINDOW_ALIGN);
        return UCS_ERR_INVALID_PARAM;
    }
    if (config->bulk_window_count == 0) {
        ucs_error("obmm: WINDOW_COUNT must be > 0");
        return UCS_ERR_INVALID_PARAM;
    }

    self->nc_region = uct_obmm_md_export_region_by_mode(md, UCT_OBMM_MAP_MODE_NC);
    self->cc_region = uct_obmm_md_export_region_by_mode(md, UCT_OBMM_MAP_MODE_CC);
    if (self->nc_region == NULL) {
        ucs_error("obmm: cannot create unified obmm iface without a local NC export region");
        return UCS_ERR_NO_DEVICE;
    }
    if (self->cc_region == NULL) {
        ucs_error("obmm: cannot create unified obmm iface without a local CC export region");
        return UCS_ERR_NO_DEVICE;
    }

    if (config->fifo_size > UINT16_MAX) {
        ucs_error("obmm: FIFO_SIZE=%u exceeds uint16_t wire format", config->fifo_size);
        return UCS_ERR_INVALID_PARAM;
    }
    if (config->fifo_elem_size > UINT16_MAX) {
        ucs_error("obmm: FIFO_ELEM_SIZE=%u exceeds uint16_t wire format",
                  config->fifo_elem_size);
        return UCS_ERR_INVALID_PARAM;
    }
    if (config->bcopy_seg_size > UINT16_MAX) {
        ucs_error("obmm: BCOPY_SEG_SIZE=%u exceeds uint16_t wire format",
                  config->bcopy_seg_size);
        return UCS_ERR_INVALID_PARAM;
    }

    nc_stride = uct_obmm_slot_stride(config->fifo_size, config->fifo_elem_size,
                                     config->bcopy_seg_size);
    if (nc_stride > UINT32_MAX) {
        ucs_error("obmm: NC slot stride %zu exceeds uint32_t "
                  "(fifo_size=%u elem=%u seg=%u)",
                  nc_stride, config->fifo_size, config->fifo_elem_size,
                  config->bcopy_seg_size);
        return UCS_ERR_INVALID_PARAM;
    }
    nc_required = uct_obmm_pool_required_size(UCT_OBMM_POOL_SLOT_COUNT,
                                              (uint32_t)nc_stride);
    nc_pool_base   = self->nc_region->base;
    nc_pool_length = self->nc_region->length;
    if (nc_required > nc_pool_length) {
        ucs_error("obmm: NC geometry does not fit in region: "
                  "fifo_size=%u elem_size=%u seg_size=%u stride=%zu "
                  "slot_count=%u required=%zu region=%zu",
                  config->fifo_size, config->fifo_elem_size,
                  config->bcopy_seg_size, nc_stride, UCT_OBMM_POOL_SLOT_COUNT,
                  nc_required, nc_pool_length);
        return UCS_ERR_INVALID_PARAM;
    }

    status = uct_obmm_cc_local_layout(&cc_stride, &cc_required, NULL);
    if (status != UCS_OK) {
        ucs_error("obmm: failed to compute built-in CC eager geometry");
        return status;
    }
    status = uct_obmm_iface_cc_pool_region(self->cc_region, &cc_pool_base,
                                           &cc_pool_length);
    if (status != UCS_OK) {
        return status;
    }
    if (cc_required > cc_pool_length) {
        ucs_error("obmm: built-in CC geometry does not fit in computed local prefix");
        return UCS_ERR_INVALID_PARAM;
    }

    ctrl_required = uct_obmm_bulk_ctrl_size(config->bulk_window_count);
    if (ctrl_required > nc_stride) {
        ucs_error("obmm: bulk control slot (%zu bytes) does not fit inside "
                  "the NC slot stride %zu; increase NC geometry or reduce WINDOW_COUNT",
                  ctrl_required, nc_stride);
        return UCS_ERR_INVALID_PARAM;
    }
    status = uct_obmm_iface_bulk_data_region(self->cc_region, &bulk_data_base,
                                             &bulk_data_offset,
                                             &bulk_data_length);
    if (status != UCS_OK) {
        return status;
    }
    if (bulk_data_offset > UINT32_MAX) {
        ucs_error("obmm: bulk data offset %zu exceeds uint32_t wire format",
                  bulk_data_offset);
        return UCS_ERR_INVALID_PARAM;
    }
    if (config->bulk_window_size > UINT32_MAX) {
        ucs_error("obmm: bulk window size %zu exceeds uint32_t wire format",
                  config->bulk_window_size);
        return UCS_ERR_INVALID_PARAM;
    }
    if (config->bulk_window_count > UINT8_MAX) {
        ucs_error("obmm: bulk window count %u exceeds uint8_t wire format",
                  config->bulk_window_count);
        return UCS_ERR_INVALID_PARAM;
    }
    if (((size_t)config->bulk_window_count * config->bulk_window_size) >
        bulk_data_length) {
        ucs_error("obmm: bulk data windows need %zu bytes but only %zu bytes "
                  "remain after the computed obmm local prefix",
                  (size_t)config->bulk_window_count * config->bulk_window_size,
                  bulk_data_length);
        return UCS_ERR_INVALID_PARAM;
    }

    UCS_CLASS_CALL_SUPER_INIT(uct_base_iface_t, &uct_obmm_iface_ops,
                              &uct_obmm_iface_internal_ops, tl_md, worker,
                              params, &config->super.super
                              UCS_STATS_ARG((params->field_mask &
                                             UCT_IFACE_PARAM_FIELD_STATS_ROOT) ?
                                            params->stats_root : NULL)
                              UCS_STATS_ARG(params->mode.device.dev_name));

    self->config.bandwidth = config->super.bandwidth;
    self->role           = role;
    self->fifo_min_poll  = config->fifo_min_poll;
    self->fifo_max_poll  = config->fifo_max_poll;
    self->fifo_poll_count = config->fifo_min_poll;
    self->fifo_prev_wnd_cons = 0;
    self->pending_quota  = config->pending_quota;
    memset(&self->nc, 0, sizeof(self->nc));
    memset(&self->cc, 0, sizeof(self->cc));
    memset(&self->bulk, 0, sizeof(self->bulk));
    ucs_list_head_init(&self->ep_list);
    ucs_arbiter_init(&self->arbiter);

    status = uct_obmm_pool_attach(nc_pool_base, nc_pool_length,
                                  UCT_OBMM_POOL_SLOT_COUNT,
                                  (uint32_t)nc_stride, &self->nc.pool);
    if (status != UCS_OK) {
        ucs_error("obmm: NC pool attach failed: %s", ucs_status_string(status));
        return status;
    }

    status = uct_obmm_pool_alloc_slot(&self->nc.pool, &self->nc.slot_index,
                                      &self->nc.recv_slot,
                                      &self->nc.generation);
    if (status != UCS_OK) {
        ucs_error("obmm: failed to allocate NC eager slot: %s",
                  ucs_status_string(status));
        return status;
    }
    self->nc.available          = 1;
    self->nc.region             = self->nc_region;
    self->nc.recv_ctl           = uct_obmm_slot_ctl(self->nc.recv_slot);
    self->nc.recv_short_active_mask =
            uct_obmm_slot_short_active_mask(self->nc.recv_slot);
    self->nc.recv_short_lanes   = uct_obmm_slot_short_lanes(self->nc.recv_slot);
    self->nc.recv_elems         = uct_obmm_slot_elems(self->nc.recv_slot);
    self->nc.recv_descs         = uct_obmm_slot_descs(self->nc.recv_slot,
                                                      config->fifo_size,
                                                      config->fifo_elem_size);
    self->nc.recv_published_tail = self->nc.recv_ctl->tail;
    self->nc.recv_short_hot_lane = UCT_OBMM_SHORT_LANE_COUNT;
    self->nc.recv_tail_batch     = 1;
    self->nc.fifo_size          = config->fifo_size;
    self->nc.fifo_mask          = config->fifo_size - 1u;
    self->nc.fifo_elem_size     = config->fifo_elem_size;
    self->nc.bcopy_seg_size     = config->bcopy_seg_size;

    status = uct_obmm_pool_alloc_slot(&self->nc.pool, &self->bulk.ctrl_slot_index,
                                      &self->bulk.ctrl_slot,
                                      &self->bulk.ctrl_generation);
    if (status != UCS_OK) {
        ucs_error("obmm: failed to allocate NC bulk-control slot: %s",
                  ucs_status_string(status));
        return status;
    }
    self->bulk.available      = 1;
    self->bulk.ctrl_hdr       = uct_obmm_bulk_ctrl_hdr(self->bulk.ctrl_slot);
    self->bulk.ctrl_descs     = uct_obmm_bulk_ctrl_descs(self->bulk.ctrl_slot);
    self->bulk.data_region    = self->cc_region;
    self->bulk.data_base      = bulk_data_base;
    self->bulk.data_offset    = bulk_data_offset;
    self->bulk.window_size    = config->bulk_window_size;
    self->bulk.window_count   = config->bulk_window_count;
    self->bulk.next_window    = 0;
    self->bulk.ctrl_hdr->magic        = UCT_OBMM_BULK_CTRL_MAGIC;
    self->bulk.ctrl_hdr->generation   = self->bulk.ctrl_generation;
    self->bulk.ctrl_hdr->cc_memid     = self->cc_region->info.memid;
    self->bulk.ctrl_hdr->window_size  = self->bulk.window_size;
    self->bulk.ctrl_hdr->window_count = self->bulk.window_count;
    self->bulk.ctrl_hdr->version      = UCT_OBMM_BULK_CTRL_VERSION;
    self->bulk.ctrl_hdr->req_seq      = 0;
    ucs_memory_bus_store_fence();

    status = uct_obmm_pool_attach(cc_pool_base, cc_pool_length,
                                  UCT_OBMM_POOL_SLOT_COUNT,
                                  (uint32_t)cc_stride, &self->cc.pool);
    if (status != UCS_OK) {
        ucs_error("obmm: CC pool attach failed: %s", ucs_status_string(status));
        return status;
    }
    status = uct_obmm_pool_alloc_slot(&self->cc.pool, &self->cc.slot_index,
                                      &self->cc.recv_slot,
                                      &self->cc.generation);
    if (status != UCS_OK) {
        ucs_error("obmm: failed to allocate CC eager slot: %s",
                  ucs_status_string(status));
        return status;
    }
    self->cc.available           = 1;
    self->cc.region              = self->cc_region;
    self->cc.recv_ctl            = uct_obmm_slot_ctl(self->cc.recv_slot);
    self->cc.recv_short_active_mask =
            uct_obmm_slot_short_active_mask(self->cc.recv_slot);
    self->cc.recv_short_lanes    = uct_obmm_slot_short_lanes(self->cc.recv_slot);
    self->cc.recv_elems          = uct_obmm_slot_elems(self->cc.recv_slot);
    self->cc.recv_descs          = uct_obmm_slot_descs(self->cc.recv_slot,
                                                       UCT_OBMM_CC_LOCAL_FIFO_SIZE,
                                                       UCT_OBMM_CC_LOCAL_FIFO_ELEM_SIZE);
    self->cc.recv_published_tail = self->cc.recv_ctl->tail;
    self->cc.recv_short_hot_lane = UCT_OBMM_SHORT_LANE_COUNT;
    self->cc.recv_tail_batch     = ucs_max(UCT_OBMM_CC_LOCAL_FIFO_SIZE / 2u, 1u);
    self->cc.fifo_size           = UCT_OBMM_CC_LOCAL_FIFO_SIZE;
    self->cc.fifo_mask           = UCT_OBMM_CC_LOCAL_FIFO_SIZE - 1u;
    self->cc.fifo_elem_size      = UCT_OBMM_CC_LOCAL_FIFO_ELEM_SIZE;
    self->cc.bcopy_seg_size      = UCT_OBMM_CC_LOCAL_BCOPY_SEG_SIZE;

    ucs_debug("%s: iface %p nc(slot=%u gen=%u stride=%zu) "
              "bulk(slot=%u gen=%u window=%zu/%u) cc(slot=%u gen=%u stride=%zu)",
              uct_obmm_iface_role_name(self->role), self, self->nc.slot_index,
              self->nc.generation, nc_stride,
              self->bulk.ctrl_slot_index, self->bulk.ctrl_generation,
              self->bulk.window_size, self->bulk.window_count,
              self->cc.slot_index, self->cc.generation, cc_stride);
    return UCS_OK;
}


static UCS_CLASS_CLEANUP_FUNC(uct_obmm_iface_t)
{
    int nc_reset = 0;

    uct_base_iface_progress_disable(&self->super.super,
                                    UCT_PROGRESS_SEND | UCT_PROGRESS_RECV);
    uct_obmm_iface_bulk_cleanup_windows(self);
    if (self->nc.pool.hdr != NULL) {
        if (self->bulk.available) {
            nc_reset |= uct_obmm_pool_free_slot(&self->nc.pool,
                                                self->bulk.ctrl_slot_index);
        }
        if (self->nc.available) {
            nc_reset |= uct_obmm_pool_free_slot(&self->nc.pool,
                                                self->nc.slot_index);
        }
        if (nc_reset) {
            uct_obmm_pool_reset(&self->nc.pool);
        }
    }
    if ((self->cc.pool.hdr != NULL) && self->cc.available &&
        uct_obmm_pool_free_slot(&self->cc.pool, self->cc.slot_index)) {
        uct_obmm_pool_reset(&self->cc.pool);
    }
    ucs_arbiter_cleanup(&self->arbiter);
}


UCS_CLASS_DEFINE(uct_obmm_iface_t, uct_base_iface_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_obmm_iface_t, uct_iface_t, uct_md_h, uct_worker_h,
                          const uct_iface_params_t*, const uct_iface_config_t*);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_obmm_iface_t, uct_iface_t);


static uct_iface_ops_t uct_obmm_iface_ops = {
    .ep_put_short             = (uct_ep_put_short_func_t)ucs_empty_function_return_unsupported,
    .ep_put_bcopy             = (uct_ep_put_bcopy_func_t)ucs_empty_function_return_unsupported,
    .ep_get_bcopy             = (uct_ep_get_bcopy_func_t)ucs_empty_function_return_unsupported,
    .ep_am_short              = uct_obmm_ep_am_short,
    .ep_am_short_iov          = (uct_ep_am_short_iov_func_t)ucs_empty_function_return_unsupported,
    .ep_am_bcopy              = uct_obmm_ep_am_bcopy,
    .ep_atomic_cswap64        = (uct_ep_atomic_cswap64_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic64_post         = (uct_ep_atomic64_post_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic64_fetch        = (uct_ep_atomic64_fetch_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic_cswap32        = (uct_ep_atomic_cswap32_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic32_post         = (uct_ep_atomic32_post_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic32_fetch        = (uct_ep_atomic32_fetch_func_t)ucs_empty_function_return_unsupported,
    .ep_pending_add           = uct_obmm_ep_pending_add,
    .ep_pending_purge         = uct_obmm_ep_pending_purge,
    .ep_flush                 = uct_base_ep_flush,
    .ep_fence                 = uct_obmm_ep_fence,
    .ep_check                 = (uct_ep_check_func_t)ucs_empty_function_return_unsupported,
    .ep_create                = UCS_CLASS_NEW_FUNC_NAME(uct_obmm_ep_t),
    .ep_destroy               = UCS_CLASS_DELETE_FUNC_NAME(uct_obmm_ep_t),
    .iface_flush              = uct_base_iface_flush,
    .iface_fence              = uct_obmm_iface_fence,
    .iface_progress_enable    = uct_base_iface_progress_enable,
    .iface_progress_disable   = uct_base_iface_progress_disable,
    .iface_progress           = uct_obmm_iface_progress,
    .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_obmm_iface_t),
    .iface_query              = uct_obmm_iface_query,
    .iface_get_device_address = uct_obmm_iface_get_device_address,
    .iface_get_address        = uct_obmm_iface_get_address,
    .iface_is_reachable       = uct_base_iface_is_reachable
};


static uct_iface_internal_ops_t uct_obmm_iface_internal_ops = {
    .iface_estimate_perf   = uct_base_iface_estimate_perf,
    .iface_vfs_refresh     = (uct_iface_vfs_refresh_func_t)ucs_empty_function,
    .ep_query              = (uct_ep_query_func_t)ucs_empty_function_return_unsupported,
    .ep_invalidate         = (uct_ep_invalidate_func_t)ucs_empty_function_return_unsupported,
    .ep_connect_to_ep_v2   = ucs_empty_function_return_unsupported,
    .iface_is_reachable_v2 = uct_obmm_iface_is_reachable_v2,
    .ep_is_connected       = uct_obmm_ep_is_connected
};


UCT_TL_DEFINE_ENTRY(&uct_obmm_component, obmm_cc,
                    uct_obmm_cc_iface_query_tl_devices,
                    uct_obmm_iface_t, "OBMM_CC_", uct_obmm_iface_config_table,
                    uct_obmm_iface_config_t);

UCT_TL_DEFINE_ENTRY(&uct_obmm_component, obmm_nc,
                    uct_obmm_nc_iface_query_tl_devices,
                    uct_obmm_iface_t, "OBMM_", uct_obmm_iface_config_table,
                    uct_obmm_iface_config_t);

void uct_obmm_init(void)
{
    uct_component_register(&uct_obmm_component);
    uct_tl_register(&uct_obmm_component, &UCT_TL_NAME(obmm_cc));
    uct_tl_register(&uct_obmm_component, &UCT_TL_NAME(obmm_nc));
}

void uct_obmm_cleanup(void)
{
    uct_tl_unregister(&UCT_TL_NAME(obmm_nc));
    uct_tl_unregister(&UCT_TL_NAME(obmm_cc));
    uct_component_unregister(&uct_obmm_component);
}
