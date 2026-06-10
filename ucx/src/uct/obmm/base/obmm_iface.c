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

#include <uct/api/v2/uct_v2.h>
#include <uct/base/uct_log.h>
#include <uct/base/uct_worker.h>
#include <ucs/arch/cpu.h>
#include <ucs/async/async.h>
#include <ucs/debug/log.h>
#include <ucs/debug/memtrack_int.h>
#include <ucs/sys/math.h>
#include <ucs/sys/ptr_arith.h>
#include <ucs/sys/sys.h>
#include <ucs/type/class.h>

#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


static uct_iface_ops_t          uct_obmm_iface_ops;
static uct_iface_internal_ops_t uct_obmm_iface_internal_ops;

#define UCT_OBMM_NC_DEVICE_NAME "memory-nc"
#define UCT_OBMM_CC_DEVICE_NAME "memory-cc"
#define UCT_OBMM_MIN_BCOPY_SEG_SIZE 64u
#define UCT_OBMM_WORKER_KEY 0x4f424d4du /* OBMM */

typedef struct uct_obmm_worker {
    uct_worker_tl_data_t super;
    uct_worker_progress_t prog;
    ucs_list_link_t       ifaces;
    unsigned              active_count;
} uct_obmm_worker_t;


static const char *uct_obmm_iface_plane_name(uct_obmm_plane_t plane)
{
    return (plane == UCT_OBMM_PLANE_CC) ? "cc" : "nc";
}


static uct_obmm_plane_t uct_obmm_iface_plane_from_tl_name(const char *tl_name)
{
    return !strcmp(tl_name, "obmm_cc") ? UCT_OBMM_PLANE_CC :
                                         UCT_OBMM_PLANE_NC;
}


static int uct_obmm_worker_cmp(uct_obmm_worker_t *worker)
{
    (void)worker;
    return 1;
}


static ucs_status_t uct_obmm_worker_init(uct_obmm_worker_t *worker)
{
    uct_worker_progress_init(&worker->prog);
    ucs_list_head_init(&worker->ifaces);
    worker->active_count = 0;
    return UCS_OK;
}


static void uct_obmm_worker_cleanup(uct_obmm_worker_t *worker)
{
    ucs_assert(worker->active_count == 0);
    ucs_assert(ucs_list_is_empty(&worker->ifaces));
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


ucs_config_field_t uct_obmm_nc_iface_config_table[] = {
    {"", "", NULL, ucs_offsetof(uct_obmm_iface_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_iface_config_table)},

    {"BW", "3400MBs",
     "Effective transport bandwidth used for UCP lane/protocol cost "
     "modeling. This is not a required knob: if the user does not set "
     "UCX_OBMM_NC_BW, obmm_nc uses this sustained default.",
     ucs_offsetof(uct_obmm_iface_config_t, super.bandwidth), UCS_CONFIG_TYPE_BW},

    {"SHORT_OVERHEAD", "1800ns",
     "Estimated per-side overhead for AM_SHORT in UCP protocol selection. "
     "The default is calibrated from the cross-node two-process OSU "
     "small-message latency.",
     ucs_offsetof(uct_obmm_iface_config_t, short_overhead),
     UCS_CONFIG_TYPE_TIME},

    {"BCOPY_OVERHEAD", "2us",
     "Estimated per-side overhead for AM_BCOPY in UCP protocol selection. "
     "OBMM bcopy is used by UCP eager/rendezvous AM fragment paths.",
     ucs_offsetof(uct_obmm_iface_config_t, bcopy_overhead),
     UCS_CONFIG_TYPE_TIME},

    {"FIFO_SIZE", "128",
     "Number of elements in the per-iface receive FIFO ring (power of 2). "
     "The shared FIFO carries both am_short and am_bcopy publications.",
     ucs_offsetof(uct_obmm_iface_config_t, fifo_size), UCS_CONFIG_TYPE_UINT},

    {"FIFO_ELEM_SIZE", "131200",
     "Size in bytes of a single FIFO element. The element contains metadata "
     "plus overlapping am_short and am_bcopy data ranges. Short starts at "
     "byte 16; bcopy starts at byte 64. Keep this stride 64-byte aligned.",
        ucs_offsetof(uct_obmm_iface_config_t, fifo_elem_size),
        UCS_CONFIG_TYPE_UINT},

    {"BCOPY_SEG_SIZE", "131072",
     "Maximum AM_BCOPY payload size advertised to UCP. Bcopy payload reuses "
     "the same per-FIFO-element allocation as short and therefore must fit "
     "after the 64-byte bcopy data offset.",
     ucs_offsetof(uct_obmm_iface_config_t, bcopy_seg_size),
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

ucs_config_field_t uct_obmm_cc_iface_config_table[] = {
    {"", "", NULL, ucs_offsetof(uct_obmm_iface_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_iface_config_table)},

    {"BW", "12300MBs",
     "Effective same-node CC bandwidth used for UCP lane/protocol cost "
     "modeling. The default is calibrated from the sustained two-process "
     "OSU large-message rate.",
     ucs_offsetof(uct_obmm_iface_config_t, super.bandwidth), UCS_CONFIG_TYPE_BW},

    {"SHORT_OVERHEAD", "100ns",
     "Estimated per-side overhead for same-node CC AM_SHORT in UCP protocol "
     "selection. The default is half of the measured two-process "
     "small-message latency.",
     ucs_offsetof(uct_obmm_iface_config_t, short_overhead),
     UCS_CONFIG_TYPE_TIME},

    {"BCOPY_OVERHEAD", "200ns",
     "Estimated per-side overhead for same-node CC AM_BCOPY. The calibrated "
     "value reflects that bcopy reuses the inline FIFO data area and does not "
     "have the previously modeled 1 us fixed cost.",
     ucs_offsetof(uct_obmm_iface_config_t, bcopy_overhead),
     UCS_CONFIG_TYPE_TIME},

    {"FIFO_SIZE", "128",
     "Number of elements in the per-iface receive FIFO ring (power of 2).",
     ucs_offsetof(uct_obmm_iface_config_t, fifo_size), UCS_CONFIG_TYPE_UINT},

    {"FIFO_ELEM_SIZE", "131200",
     "Size in bytes of a single same-node CC FIFO element. This controls "
     "the overlapping am_short/am_bcopy data ranges and should remain "
     "64-byte aligned.",
        ucs_offsetof(uct_obmm_iface_config_t, fifo_elem_size),
        UCS_CONFIG_TYPE_UINT},

    {"BCOPY_SEG_SIZE", "131072",
     "Maximum same-node CC AM_BCOPY payload size advertised to UCP. Payload "
     "reuses the FIFO element data area.",
     ucs_offsetof(uct_obmm_iface_config_t, bcopy_seg_size),
     UCS_CONFIG_TYPE_UINT},

    {"FIFO_MIN_POLL", "16",
     "Minimal receive completions to drain in one progress() call.",
     ucs_offsetof(uct_obmm_iface_config_t, fifo_min_poll),
      UCS_CONFIG_TYPE_ULUNITS},

    {"FIFO_MAX_POLL", "16",
     "Maximal receive completions to drain in one progress() call.",
     ucs_offsetof(uct_obmm_iface_config_t, fifo_max_poll),
        UCS_CONFIG_TYPE_ULUNITS},

    {"PENDING_QUOTA", "1",
     "How many pending send retries may be dispatched during iface progress.",
     ucs_offsetof(uct_obmm_iface_config_t, pending_quota),
     UCS_CONFIG_TYPE_UINT},

     {NULL}
};


static ucs_status_t
uct_obmm_iface_query_plane_devices(uct_md_h tl_md, uct_obmm_plane_t plane,
                                   const char *dev_name,
                                   uct_tl_device_resource_t **tl_devices_p,
                                   unsigned *num_tl_devices_p)
{
    uct_obmm_md_t *md = ucs_derived_of(tl_md, uct_obmm_md_t);

    if (!uct_obmm_md_has_plane(md, plane)) {
        *tl_devices_p     = NULL;
        *num_tl_devices_p = 0;
        return UCS_ERR_NO_DEVICE;
    }

    return uct_single_device_resource(tl_md, dev_name,
                                      UCT_DEVICE_TYPE_SHM,
                                      UCS_SYS_DEVICE_ID_UNKNOWN, tl_devices_p,
                                      num_tl_devices_p);
}

ucs_status_t
uct_obmm_nc_iface_query_tl_devices(uct_md_h md,
                                   uct_tl_device_resource_t **tl_devices_p,
                                   unsigned *num_tl_devices_p)
{
    return uct_obmm_iface_query_plane_devices(md, UCT_OBMM_PLANE_NC,
                                              UCT_OBMM_NC_DEVICE_NAME,
                                              tl_devices_p, num_tl_devices_p);
}

ucs_status_t
uct_obmm_cc_iface_query_tl_devices(uct_md_h md,
                                   uct_tl_device_resource_t **tl_devices_p,
                                   unsigned *num_tl_devices_p)
{
    return uct_obmm_iface_query_plane_devices(md, UCT_OBMM_PLANE_CC,
                                              UCT_OBMM_CC_DEVICE_NAME,
                                              tl_devices_p, num_tl_devices_p);
}


static ucs_status_t uct_obmm_iface_query(uct_iface_h tl_iface,
                                         uct_iface_attr_t *attr)
{
    uct_obmm_iface_t *iface     = ucs_derived_of(tl_iface, uct_obmm_iface_t);
    size_t            max_short = uct_obmm_fifo_max_short(
                                  iface->fifo_elem_size);

    uct_base_iface_query(&iface->super, attr);
    attr->cap.flags              = UCT_IFACE_FLAG_AM_SHORT         |
                                   UCT_IFACE_FLAG_AM_BCOPY         |
                                   UCT_IFACE_FLAG_PENDING          |
                                   UCT_IFACE_FLAG_CONNECT_TO_IFACE |
                                   UCT_IFACE_FLAG_CB_SYNC;
    if (iface->plane == UCT_OBMM_PLANE_NC) {
        attr->cap.flags         |= UCT_IFACE_FLAG_INTER_NODE;
    }
    attr->iface_addr_len         = sizeof(uct_obmm_iface_addr_t);
    attr->device_addr_len        = sizeof(uct_obmm_device_addr_t);
    attr->ep_addr_len            = 0;
    attr->max_conn_priv          = 0;

    /* UCT contract: max_short is total bytes the caller may pass as
     * (header + payload). obmm stores am_short inline in the shared FIFO
     * element starting at elem->header. */

    attr->cap.am.max_short       = max_short;
    attr->cap.am.max_bcopy       = iface->bcopy_seg_size;
    attr->cap.am.min_zcopy       = 0;
    attr->cap.am.max_zcopy       = 0;
    attr->cap.am.max_iov         = 0;
    attr->cap.am.max_hdr         = 0;
    attr->cap.am.opt_zcopy_align = 1;
    attr->cap.am.align_mtu       = 1;

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
    attr->overhead               = iface->config.short_overhead;
    attr->priority               = 0;
    return UCS_OK;
}


static ucs_status_t
uct_obmm_iface_estimate_perf(uct_iface_h tl_iface,
                             uct_perf_attr_t *perf_attr)
{
    uct_obmm_iface_t *iface = ucs_derived_of(tl_iface, uct_obmm_iface_t);
    uct_ep_operation_t op   = UCT_ATTR_VALUE(PERF, perf_attr, operation,
                                             OPERATION, UCT_EP_OP_LAST);
    double send_pre_overhead = iface->config.short_overhead;
    double recv_overhead     = iface->config.short_overhead;
    double bandwidth         = iface->config.bandwidth;

    switch (op) {
    case UCT_EP_OP_AM_SHORT:
        break;
    case UCT_EP_OP_AM_BCOPY:
        send_pre_overhead = iface->config.bcopy_overhead;
        recv_overhead     = iface->config.bcopy_overhead;
        break;
    default:
        break;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_SEND_PRE_OVERHEAD) {
        perf_attr->send_pre_overhead = send_pre_overhead;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_SEND_POST_OVERHEAD) {
        perf_attr->send_post_overhead = 0;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_RECV_OVERHEAD) {
        perf_attr->recv_overhead = recv_overhead;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_BANDWIDTH) {
        perf_attr->bandwidth.dedicated = bandwidth;
        perf_attr->bandwidth.shared    = 0;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_LATENCY) {
        perf_attr->latency = UCS_LINEAR_FUNC_ZERO;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_MAX_INFLIGHT_EPS) {
        perf_attr->max_inflight_eps = SIZE_MAX;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_FLAGS) {
        perf_attr->flags = 0;
    }

    return UCS_OK;
}


static ucs_status_t
uct_obmm_iface_get_device_address(uct_iface_h tl_iface,
                                  uct_device_addr_t *addr)
{
    uct_obmm_iface_t       *iface = ucs_derived_of(tl_iface, uct_obmm_iface_t);
    uct_obmm_device_addr_t *daddr = (uct_obmm_device_addr_t*)addr;

    daddr->exporter_dcna    = iface->region->info.exporter_dcna;
    daddr->exporter_deid_hi = iface->region->info.exporter_deid.hi;
    daddr->exporter_deid_lo = iface->region->info.exporter_deid.lo;
    return UCS_OK;
}


static ucs_status_t uct_obmm_iface_get_address(uct_iface_h tl_iface,
                                               uct_iface_addr_t *addr)
{
    uct_obmm_iface_t      *iface = ucs_derived_of(tl_iface, uct_obmm_iface_t);
    uct_obmm_iface_addr_t *iaddr = (uct_obmm_iface_addr_t*)addr;

    iaddr->slot_index       = iface->slot_index;
    iaddr->generation       = iface->generation;
    iaddr->pid              = (uint32_t)getpid();
    iaddr->plane            = iface->plane;
    iaddr->slot_count       = UCT_OBMM_POOL_SLOT_COUNT;
    iaddr->short_lane_count = UCT_OBMM_SHORT_LANE_COUNT;
    iaddr->wire_format      = UCT_OBMM_WIRE_FORMAT_CURRENT;
    iaddr->fifo_size        = iface->fifo_size;
    iaddr->fifo_elem_size   = iface->fifo_elem_size;
    iaddr->bcopy_seg_size   = iface->bcopy_seg_size;
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
    uct_obmm_eid_t                eid;
    uct_obmm_region_t            *export_r;

    if (!uct_iface_is_reachable_params_addrs_valid(params)) {
        return 0;
    }

    daddr = (const uct_obmm_device_addr_t*)params->device_addr;
    iaddr = (const uct_obmm_iface_addr_t*)params->iface_addr;
    if ((daddr == NULL) || (iaddr == NULL)) {
        uct_iface_fill_info_str_buf(params, "missing device or iface address");
        return 0;
    }
    if ((params->field_mask & UCT_IFACE_IS_REACHABLE_FIELD_IFACE_ADDR_LENGTH) &&
        (params->iface_addr_length < sizeof(*iaddr))) {
        uct_iface_fill_info_str_buf(params,
                                    "OBMM iface address too short: peer=%zu "
                                    "local=%zu", params->iface_addr_length,
                                    sizeof(*iaddr));
        return 0;
    }

    if ((iaddr->slot_count != UCT_OBMM_POOL_SLOT_COUNT) ||
        (iaddr->short_lane_count != UCT_OBMM_SHORT_LANE_COUNT) ||
        (iaddr->plane != iface->plane) ||
        (iaddr->wire_format != UCT_OBMM_WIRE_FORMAT_CURRENT) ||
        (iaddr->fifo_size != iface->fifo_size) ||
        (iaddr->fifo_elem_size != iface->fifo_elem_size) ||
        (iaddr->bcopy_seg_size != iface->bcopy_seg_size)) {
        uct_iface_fill_info_str_buf(params,
                                    "incompatible OBMM geometry "
                                    "(peer plane=%u slots=%u lanes=%u wire=%u "
                                    "fifo=%u elem=%u seg=%u, local plane=%u "
                                    "slots=%u lanes=%u wire=%u fifo=%u "
                                    "elem=%u seg=%u)",
                                    iaddr->plane,
                                    iaddr->slot_count,
                                    iaddr->short_lane_count,
                                    iaddr->wire_format,
                                    iaddr->fifo_size, iaddr->fifo_elem_size,
                                    iaddr->bcopy_seg_size,
                                    iface->plane,
                                    UCT_OBMM_POOL_SLOT_COUNT,
                                    UCT_OBMM_SHORT_LANE_COUNT,
                                    UCT_OBMM_WIRE_FORMAT_CURRENT,
                                    iface->fifo_size, iface->fifo_elem_size,
                                    iface->bcopy_seg_size);
        return 0;
    }

    eid.hi = daddr->exporter_deid_hi;
    eid.lo = daddr->exporter_deid_lo;

    if (iface->plane == UCT_OBMM_PLANE_CC) {
        export_r = uct_obmm_md_export_region(md, UCT_OBMM_PLANE_CC);
        if ((export_r != NULL) &&
            (export_r->info.exporter_dcna == daddr->exporter_dcna) &&
            (export_r->info.exporter_deid.hi == eid.hi) &&
            (export_r->info.exporter_deid.lo == eid.lo)) {
            return uct_iface_scope_is_reachable(tl_iface, params);
        }

        uct_iface_fill_info_str_buf(params,
                                    "peer is not on the local CC export "
                                    "dcna=0x%lx deid=0x%lx:0x%lx",
                                    (unsigned long)daddr->exporter_dcna,
                                    (unsigned long)eid.hi,
                                    (unsigned long)eid.lo);
        return 0;
    }

    if (uct_obmm_md_find_import_region(md, UCT_OBMM_PLANE_NC,
                                       daddr->exporter_dcna, &eid) != NULL) {
        return uct_iface_scope_is_reachable(tl_iface, params);
    }

    if (uct_obmm_md_allow_nc_local_loopback(md) &&
        (uct_obmm_md_find_region(md, UCT_OBMM_PLANE_NC,
                                 daddr->exporter_dcna, &eid) != NULL)) {
        return uct_iface_scope_is_reachable(tl_iface, params);
    }

    uct_iface_fill_info_str_buf(params,
                                "no mapped remote NC import or standalone "
                                "local NC export for peer "
                                "dcna=0x%lx deid=0x%lx:0x%lx",
                                (unsigned long)daddr->exporter_dcna,
                                (unsigned long)eid.hi, (unsigned long)eid.lo);
    return 0;
}


static UCS_F_ALWAYS_INLINE void
uct_obmm_iface_load_fence(uct_obmm_iface_t *iface)
{
    if (iface->plane == UCT_OBMM_PLANE_CC) {
        ucs_memory_cpu_load_fence();
    } else {
        ucs_memory_bus_load_fence();
    }
}


static UCS_F_ALWAYS_INLINE void
uct_obmm_iface_full_fence(uct_obmm_iface_t *iface)
{
    if (iface->plane == UCT_OBMM_PLANE_CC) {
        ucs_memory_cpu_fence();
    } else {
        uct_obmm_bus_full_fence();
    }
}


static unsigned uct_obmm_iface_progress_one(uct_obmm_iface_t *iface)
{
    unsigned                 polled = 0;
    unsigned                 pending_progress = 0;
    uct_obmm_fifo_element_t *elem;
    uint8_t                  flags;
    uint8_t                  expected_owner;
    size_t                   max_poll = iface->fifo_poll_count;

    while (polled < max_poll) {
        elem = uct_obmm_slot_elem(iface->recv_elems, iface->read_index,
                                  iface->fifo_mask, iface->fifo_elem_size);

        /* Owner bit alternates each lap of the ring; pass 0 expects 1, pass
         * 1 expects 0, etc. Combined with zero-fill on slot allocation, this
         * means an unwritten slot reads as flags==0 and is correctly skipped
         * on the very first lap. */
        expected_owner = (iface->read_index & iface->fifo_size) ?
                         0u : UCT_OBMM_FIFO_ELEM_FLAG_OWNER;

        flags = elem->flags;
        if ((flags & UCT_OBMM_FIFO_ELEM_FLAG_OWNER) != expected_owner) {
            break;
        }

        uct_obmm_iface_load_fence(iface);

        if (elem->generation != iface->generation) {
            /* Stale write from a previous slot owner (we were torn down and
             * re-allocated this slot). Drop silently. */
            ucs_trace_data("obmm: drop stale elem (gen=%u expected=%u) "
                           "at idx=%lu", elem->generation, iface->generation,
                           (unsigned long)iface->read_index);
        } else if (flags & UCT_OBMM_FIFO_ELEM_FLAG_BCOPY) {
            /* am_bcopy: payload starts at the FIFO element's 64-byte-aligned
             * bcopy offset. The plane-specific load fence above orders this
             * load with respect to the sender's matching store fence + flag
             * write. */
            if (ucs_unlikely(elem->length > iface->bcopy_seg_size)) {
                ucs_error("obmm: invalid bcopy length %u at idx=%lu "
                          "(seg_size=%u gen=%u expected=%u)", elem->length,
                          (unsigned long)iface->read_index,
                          iface->bcopy_seg_size, elem->generation,
                          iface->generation);
            } else {
                uct_iface_invoke_am(&iface->super, elem->am_id,
                                    uct_obmm_fifo_elem_bcopy_data(elem),
                                    elem->length, 0);
            }
        } else {
            if (ucs_unlikely((elem->length < sizeof(elem->header)) ||
                             (elem->length >
                              uct_obmm_fifo_max_short(iface->fifo_elem_size)))) {
                ucs_error("obmm: invalid FIFO short length %u at idx=%lu "
                          "(max_short=%u gen=%u expected=%u)",
                          elem->length, (unsigned long)iface->read_index,
                          uct_obmm_fifo_max_short(iface->fifo_elem_size),
                          elem->generation, iface->generation);
            } else {
                uct_iface_invoke_am(&iface->super, elem->am_id,
                                    uct_obmm_fifo_elem_short_data(elem),
                                    elem->length, 0);
            }
        }

        iface->read_index++;
        polled++;
    }

    uct_obmm_iface_fifo_window_adjust(iface, polled);

    if (polled > 0) {
        /* Full release fence: orders the AM handler's LOADS from FIFO payload
         * payload BEFORE the STORE that publishes the new tail. On NC this
         * is a full bus-domain fence; on same-node CC it is a CPU fence. A
         * plain store fence would let a sender observe the advanced tail and
         * overwrite the FIFO entry while we still have outstanding loads in
         * flight. */
        uct_obmm_iface_full_fence(iface);
        iface->recv_ctl->tail = iface->read_index;
    }

    /* Drain any UCP requests waiting on TX backpressure. The peer-side
     * tail advance we just published may also have freed slots that *our*
     * pending eps have been waiting for; dispatch with a fresh head/tail
     * snapshot so queued retries see the latest state. */
    ucs_arbiter_dispatch(&iface->arbiter, 1, uct_obmm_ep_process_pending,
                         &pending_progress);

    return polled + pending_progress;
}


static unsigned uct_obmm_worker_progress(void *arg)
{
    uct_obmm_worker_t *worker = (uct_obmm_worker_t*)arg;
    uct_obmm_iface_t  *iface;
    unsigned           count = 0;

    ucs_list_for_each(iface, &worker->ifaces, worker_list) {
        count += uct_obmm_iface_progress_one(iface);
    }

    return count;
}


static unsigned uct_obmm_iface_progress(uct_iface_h tl_iface)
{
    return uct_obmm_iface_progress_one(ucs_derived_of(tl_iface,
                                                      uct_obmm_iface_t));
}


static void uct_obmm_iface_progress_enable(uct_iface_h tl_iface,
                                           unsigned flags)
{
    uct_obmm_iface_t *iface = ucs_derived_of(tl_iface, uct_obmm_iface_t);
    uct_base_iface_t *base  = &iface->super;
    int               need_add = 0;

    flags &= ~UCT_PROGRESS_THREAD_SAFE;
    if (flags == 0) {
        return;
    }

    if (base->progress_flags == 0) {
        UCS_ASYNC_BLOCK(base->worker->async);
        if (!iface->progress_active) {
            ucs_list_add_tail(&iface->worker_ctx->ifaces,
                              &iface->worker_list);
            iface->progress_active = 1;
            need_add = (iface->worker_ctx->active_count++ == 0);
        }
        UCS_ASYNC_UNBLOCK(base->worker->async);

        if (need_add) {
            uct_worker_progress_add_safe(base->worker,
                                         uct_obmm_worker_progress,
                                         iface->worker_ctx,
                                         &iface->worker_ctx->prog);
        }
    }

    base->progress_flags |= flags;
}


static void uct_obmm_iface_progress_disable(uct_iface_h tl_iface,
                                            unsigned flags)
{
    uct_obmm_iface_t *iface = ucs_derived_of(tl_iface, uct_obmm_iface_t);
    uct_base_iface_t *base  = &iface->super;
    int               need_remove = 0;

    flags &= ~UCT_PROGRESS_THREAD_SAFE;
    if (flags == 0) {
        return;
    }

    base->progress_flags &= ~flags;
    if (base->progress_flags != 0) {
        return;
    }

    UCS_ASYNC_BLOCK(base->worker->async);
    if (iface->progress_active) {
        ucs_list_del(&iface->worker_list);
        iface->progress_active = 0;
        ucs_assert(iface->worker_ctx->active_count > 0);
        need_remove = (--iface->worker_ctx->active_count == 0);
    }
    UCS_ASYNC_UNBLOCK(base->worker->async);

    if (need_remove) {
        uct_worker_progress_remove(base->worker, &iface->worker_ctx->prog);
    }
}


static ucs_status_t uct_obmm_iface_fence(uct_iface_h tl_iface, unsigned flags)
{
    (void)flags;
    ucs_memory_cpu_fence();
    UCT_TL_IFACE_STAT_FENCE(ucs_derived_of(tl_iface, uct_base_iface_t));
    return UCS_OK;
}


static ucs_status_t uct_obmm_iface_flush(uct_iface_h tl_iface, unsigned flags,
                                         uct_completion_t *comp)
{
    (void)flags;
    (void)comp;
    UCT_TL_IFACE_STAT_FLUSH(ucs_derived_of(tl_iface, uct_base_iface_t));
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
    uct_obmm_region_t       *region;
    uct_obmm_worker_t       *worker_ctx;
    uct_obmm_plane_t         plane;
    size_t                   stride;
    size_t                   required;
    ucs_status_t             status;

    self->pool.hdr            = NULL;
    self->slot_index          = UINT32_MAX;
    self->worker_ctx          = NULL;
    self->base_initialized    = 0;
    self->arbiter_initialized = 0;
    self->progress_active     = 0;

    UCT_CHECK_PARAM(params->field_mask & UCT_IFACE_PARAM_FIELD_OPEN_MODE,
                    "UCT_IFACE_PARAM_FIELD_OPEN_MODE is not defined");
    if (!(params->open_mode & UCT_IFACE_OPEN_MODE_DEVICE)) {
        ucs_error("only UCT_IFACE_OPEN_MODE_DEVICE is supported");
        return UCS_ERR_UNSUPPORTED;
    }
    plane = uct_obmm_iface_plane_from_tl_name(params->mode.device.tl_name);

    if (config->fifo_size == 0) {
        ucs_error("obmm: FIFO_SIZE must be > 0");
        return UCS_ERR_INVALID_PARAM;
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
    if (config->super.bandwidth <= 1.0) {
        ucs_error("obmm: BW must be positive");
        return UCS_ERR_INVALID_PARAM;
    }
    if ((config->short_overhead < 0.0) ||
        (config->bcopy_overhead < 0.0)) {
        ucs_error("obmm: performance overhead estimates must be non-negative");
        return UCS_ERR_INVALID_PARAM;
    }
    if (!ucs_is_pow2(config->fifo_size)) {
        ucs_error("obmm: FIFO_SIZE (%u) must be a power of 2",
                  config->fifo_size);
        return UCS_ERR_INVALID_PARAM;
    }
    if (config->fifo_elem_size <= uct_obmm_fifo_bcopy_data_offset()) {
        ucs_error("obmm: FIFO_ELEM_SIZE (%u) must be > %u",
                  config->fifo_elem_size,
                  uct_obmm_fifo_bcopy_data_offset());
        return UCS_ERR_INVALID_PARAM;
    }
    if (config->bcopy_seg_size < UCT_OBMM_MIN_BCOPY_SEG_SIZE) {
        ucs_error("obmm: BCOPY_SEG_SIZE (%u) is too small; UCP requires "
                  "AM bcopy transports to provide at least %u bytes",
                  config->bcopy_seg_size, UCT_OBMM_MIN_BCOPY_SEG_SIZE);
        return UCS_ERR_INVALID_PARAM;
    }
    if (config->bcopy_seg_size >
        uct_obmm_fifo_max_bcopy(config->fifo_elem_size)) {
        ucs_error("obmm: BCOPY_SEG_SIZE (%u) must fit in FIFO data "
                  "capacity %u (FIFO_ELEM_SIZE=%u)",
                  config->bcopy_seg_size,
                  uct_obmm_fifo_max_bcopy(config->fifo_elem_size),
                  config->fifo_elem_size);
        return UCS_ERR_INVALID_PARAM;
    }

    region = uct_obmm_md_export_region(md, plane);
    if (region == NULL) {
        ucs_error("obmm: cannot create %s iface; this MD has no local %s "
                  "export region", params->mode.device.tl_name,
                  uct_obmm_iface_plane_name(plane));
        return UCS_ERR_NO_DEVICE;
    }

    /* Compute slot stride as size_t, then validate it fits in u32 (the
     * pool header field is u32) AND that the total region budget covers
     * slot_count slots. Failing here is preferred over silently capping
     * BCOPY_SEG_SIZE — UCP would happily make protocol decisions based
     * on a quietly reduced max_bcopy. bcopy payload reuses the FIFO element
     * data area and does not add a second per-entry desc allocation. */
    stride = uct_obmm_slot_stride(config->fifo_size, config->fifo_elem_size,
                                  config->bcopy_seg_size);
    if (stride > UINT32_MAX) {
        ucs_error("obmm: slot stride %zu exceeds uint32_t (fifo_size=%u "
                  "elem=%u seg=%u); reduce one of the geometry knobs",
                  stride, config->fifo_size, config->fifo_elem_size,
                  config->bcopy_seg_size);
        return UCS_ERR_INVALID_PARAM;
    }
    required = uct_obmm_pool_required_size(UCT_OBMM_POOL_SLOT_COUNT,
                                           (uint32_t)stride);
    if (required > region->length) {
        ucs_error("obmm: geometry does not fit in region: "
                  "fifo_size=%u elem_size=%u seg_size=%u stride=%zu "
                  "slot_count=%u required=%zu region=%zu. "
                  "Reduce the matching UCX_OBMM_%s_BCOPY_SEG_SIZE, "
                  "UCX_OBMM_%s_FIFO_SIZE, or UCX_OBMM_%s_FIFO_ELEM_SIZE.",
                  config->fifo_size, config->fifo_elem_size,
                  config->bcopy_seg_size, stride,
                  UCT_OBMM_POOL_SLOT_COUNT, required, region->length,
                  (plane == UCT_OBMM_PLANE_CC) ? "CC" : "NC",
                  (plane == UCT_OBMM_PLANE_CC) ? "CC" : "NC",
                  (plane == UCT_OBMM_PLANE_CC) ? "CC" : "NC");
        return UCS_ERR_INVALID_PARAM;
    }

    UCS_CLASS_CALL_SUPER_INIT(uct_base_iface_t, &uct_obmm_iface_ops,
                              &uct_obmm_iface_internal_ops, tl_md, worker,
                              params, &config->super.super
                              UCS_STATS_ARG((params->field_mask &
                                             UCT_IFACE_PARAM_FIELD_STATS_ROOT) ?
                                            params->stats_root : NULL)
                              UCS_STATS_ARG(params->mode.device.dev_name));
    self->base_initialized          = 1;

    self->plane                    = plane;
    self->region                   = region;
    self->config.bandwidth         = config->super.bandwidth;
    self->config.short_overhead    = config->short_overhead;
    self->config.bcopy_overhead    = config->bcopy_overhead;
    self->fifo_size                = config->fifo_size;
    self->fifo_mask                = config->fifo_size - 1u;
    self->fifo_elem_size           = config->fifo_elem_size;
    self->bcopy_seg_size           = config->bcopy_seg_size;
    self->fifo_min_poll            = config->fifo_min_poll;
    self->fifo_max_poll            = config->fifo_max_poll;
    self->fifo_poll_count          = config->fifo_min_poll;
    self->fifo_prev_wnd_cons = 0;
    self->pending_quota  = config->pending_quota;
    self->read_index     = 0;
    ucs_arbiter_init(&self->arbiter);
    self->arbiter_initialized = 1;

    worker_ctx = uct_worker_tl_data_get(self->super.worker,
                                        UCT_OBMM_WORKER_KEY,
                                        uct_obmm_worker_t,
                                        uct_obmm_worker_cmp,
                                        uct_obmm_worker_init);
    if (UCS_PTR_IS_ERR(worker_ctx)) {
        status = UCS_PTR_STATUS(worker_ctx);
        return status;
    }
    self->worker_ctx = worker_ctx;

    status = uct_obmm_pool_attach(region->base, region->length,
                                  UCT_OBMM_POOL_SLOT_COUNT,
                                  (uint32_t)stride, &self->pool);
    if (status != UCS_OK) {
        ucs_error("obmm: pool attach failed: %s", ucs_status_string(status));
        goto err_put_worker;
    }

    status = uct_obmm_pool_alloc_slot(&self->pool, &self->slot_index,
                                      &self->recv_slot, &self->generation);
    if (status != UCS_OK) {
        ucs_error("obmm: failed to allocate FIFO slot: %s",
                  ucs_status_string(status));
        goto err_put_worker;
    }

    self->recv_ctl   = uct_obmm_slot_ctl(self->recv_slot);
    self->recv_elems = uct_obmm_slot_elems(self->recv_slot);

    /* recv_slot was zeroed by pool_alloc_slot, so head/tail/all element
     * flags are zero. read_index starts at 0, expected owner bit on the
     * first lap is 1; uninitialized zero correctly reads as "not yet
     * written". */

    ucs_debug("obmm: %s iface %p attached to region %p slot=%u gen=%u "
              "fifo_size=%u elem_size=%u seg_size=%u stride=%zu",
              uct_obmm_iface_plane_name(self->plane), self, region->base,
              self->slot_index, self->generation,
              self->fifo_size, self->fifo_elem_size, self->bcopy_seg_size,
              stride);
    return UCS_OK;

err_put_worker:
    uct_worker_tl_data_put(self->worker_ctx, uct_obmm_worker_cleanup);
    self->worker_ctx = NULL;
    return status;
}


static UCS_CLASS_CLEANUP_FUNC(uct_obmm_iface_t)
{
    if (self->base_initialized) {
        uct_obmm_iface_progress_disable(&self->super.super,
                                        UCT_PROGRESS_SEND |
                                        UCT_PROGRESS_RECV);
    }
    if ((self->slot_index != UINT32_MAX) &&
        (self->pool.hdr != NULL) &&
        uct_obmm_pool_free_slot(&self->pool, self->slot_index)) {
        uct_obmm_pool_reset(&self->pool);
    }
    /* All eps were destroyed before iface cleanup (UCX framework
     * contract; mm relies on the same), so the arbiter is empty. */
    if (self->arbiter_initialized) {
        ucs_arbiter_cleanup(&self->arbiter);
        self->arbiter_initialized = 0;
    }
    if (self->worker_ctx != NULL) {
        uct_worker_tl_data_put(self->worker_ctx, uct_obmm_worker_cleanup);
        self->worker_ctx = NULL;
    }
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
    .ep_am_zcopy              = (uct_ep_am_zcopy_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic_cswap64        = (uct_ep_atomic_cswap64_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic64_post         = (uct_ep_atomic64_post_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic64_fetch        = (uct_ep_atomic64_fetch_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic_cswap32        = (uct_ep_atomic_cswap32_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic32_post         = (uct_ep_atomic32_post_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic32_fetch        = (uct_ep_atomic32_fetch_func_t)ucs_empty_function_return_unsupported,
    .ep_pending_add           = uct_obmm_ep_pending_add,
    .ep_pending_purge         = uct_obmm_ep_pending_purge,
    .ep_flush                 = uct_obmm_ep_flush,
    .ep_fence                 = uct_obmm_ep_fence,
    .ep_check                 = (uct_ep_check_func_t)ucs_empty_function_return_unsupported,
    .ep_create                = UCS_CLASS_NEW_FUNC_NAME(uct_obmm_ep_t),
    .ep_destroy               = UCS_CLASS_DELETE_FUNC_NAME(uct_obmm_ep_t),
    .iface_flush              = uct_obmm_iface_flush,
    .iface_fence              = uct_obmm_iface_fence,
    .iface_progress_enable    = uct_obmm_iface_progress_enable,
    .iface_progress_disable   = uct_obmm_iface_progress_disable,
    .iface_progress           = uct_obmm_iface_progress,
    .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_obmm_iface_t),
    .iface_query              = uct_obmm_iface_query,
    .iface_get_device_address = uct_obmm_iface_get_device_address,
    .iface_get_address        = uct_obmm_iface_get_address,
    .iface_is_reachable       = uct_base_iface_is_reachable
};


static uct_iface_internal_ops_t uct_obmm_iface_internal_ops = {
    .iface_estimate_perf   = uct_obmm_iface_estimate_perf,
    .iface_vfs_refresh     = (uct_iface_vfs_refresh_func_t)ucs_empty_function,
    .ep_query              = (uct_ep_query_func_t)ucs_empty_function_return_unsupported,
    .ep_invalidate         = (uct_ep_invalidate_func_t)ucs_empty_function_return_unsupported,
    .ep_connect_to_ep_v2   = ucs_empty_function_return_unsupported,
    .iface_is_reachable_v2 = uct_obmm_iface_is_reachable_v2,
    .ep_is_connected       = uct_obmm_ep_is_connected
};


UCT_TL_DEFINE_ENTRY(&uct_obmm_component, obmm_nc,
                    uct_obmm_nc_iface_query_tl_devices,
                    uct_obmm_iface_t, "OBMM_NC_",
                    uct_obmm_nc_iface_config_table,
                    uct_obmm_iface_config_t);

UCT_TL_DEFINE_ENTRY(&uct_obmm_component, obmm_cc,
                    uct_obmm_cc_iface_query_tl_devices,
                    uct_obmm_iface_t, "OBMM_CC_",
                    uct_obmm_cc_iface_config_table,
                    uct_obmm_iface_config_t);

void UCS_F_CTOR uct_obmm_init(void)
{
    uct_component_register(&uct_obmm_component);
    uct_tl_register(&uct_obmm_component, &UCT_TL_NAME(obmm_nc));
    uct_tl_register(&uct_obmm_component, &UCT_TL_NAME(obmm_cc));
}

void UCS_F_DTOR uct_obmm_cleanup(void)
{
    uct_tl_unregister(&UCT_TL_NAME(obmm_cc));
    uct_tl_unregister(&UCT_TL_NAME(obmm_nc));
    uct_component_unregister(&uct_obmm_component);
}
