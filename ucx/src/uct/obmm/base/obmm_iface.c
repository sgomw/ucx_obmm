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
#include <ucs/arch/cpu.h>
#include <ucs/debug/log.h>
#include <ucs/debug/memtrack_int.h>
#include <ucs/sys/math.h>
#include <ucs/sys/ptr_arith.h>
#include <ucs/sys/sys.h>
#include <ucs/type/class.h>

#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>


static uct_iface_ops_t          uct_obmm_iface_ops;
static uct_iface_internal_ops_t uct_obmm_iface_internal_ops;

#define UCT_OBMM_DEVICE_NAME "memory"
#define UCT_OBMM_MIN_BCOPY_SEG_SIZE 64u


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


ucs_config_field_t uct_obmm_iface_config_table[] = {
    {"", "", NULL, ucs_offsetof(uct_obmm_iface_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_iface_config_table)},

    {"BW", "3400MBs",
     "Effective transport bandwidth used for UCP lane/protocol cost "
     "modeling. This is not a required knob: if the user does not set "
     "UCX_OBMM_BW, obmm uses this sustained default.",
     ucs_offsetof(uct_obmm_iface_config_t, super.bandwidth), UCS_CONFIG_TYPE_BW},

    {"SHORT_OVERHEAD", "100ns",
     "Estimated per-side overhead for AM_SHORT in UCP protocol selection.",
     ucs_offsetof(uct_obmm_iface_config_t, short_overhead),
     UCS_CONFIG_TYPE_TIME},

    {"BCOPY_OVERHEAD", "2us",
     "Estimated per-side overhead for AM_BCOPY in UCP protocol selection. "
     "OBMM bcopy is a fallback/control path and uses small fragments.",
     ucs_offsetof(uct_obmm_iface_config_t, bcopy_overhead),
     UCS_CONFIG_TYPE_TIME},

    {"CC_BW", "6000MBs",
     "Estimated cacheable CC staged AM_ZCOPY bandwidth for UCP protocol "
     "selection. This models the current staged path, including CPU copies; "
     "calibrate on target before treating it as hardware CC bandwidth.",
     ucs_offsetof(uct_obmm_iface_config_t, cc_bandwidth),
     UCS_CONFIG_TYPE_BW},

    {"CC_ZCOPY_OVERHEAD", "900us",
     "Estimated per-side ownership/staging overhead for CC AM_ZCOPY in UCP "
     "protocol selection. Default is intentionally high so the current "
     "receiver-owned implementation is selected only for large messages where "
     "it beats NC eager/rendezvous paths under high process counts.",
     ucs_offsetof(uct_obmm_iface_config_t, cc_zcopy_overhead),
     UCS_CONFIG_TYPE_TIME},

    {"FIFO_SIZE", "64",
     "Number of elements in the per-iface receive FIFO ring (power of 2). "
     "The shared FIFO carries both am_short and am_bcopy publications.",
     ucs_offsetof(uct_obmm_iface_config_t, fifo_size), UCS_CONFIG_TYPE_UINT},

    {"FIFO_ELEM_SIZE", "520128",
     "Size in bytes of a single FIFO element. This controls am_short capacity: "
     "inline short data is stored in the FIFO element as [header|payload], "
     "while bcopy data uses the paired descriptor area. Keep this stride "
     "64-byte aligned.",
        ucs_offsetof(uct_obmm_iface_config_t, fifo_elem_size),
        UCS_CONFIG_TYPE_UINT},

    {"BCOPY_SEG_SIZE", "4096",
     "Size in bytes of each per-FIFO-elem bcopy descriptor. This is "
     "advertised as max_bcopy. Keep this small: bcopy is only a UCP "
     "wireup/control/fallback path in the short-first design, while "
     "performance-sensitive eager payloads should fit inline in FIFO short. "
     "Per-slot shared FIFO footprint = FIFO_SIZE * "
     "(FIFO_ELEM_SIZE + BCOPY_SEG_SIZE).",
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

    {"CC_MIN_ZCOPY", "2M",
     "NC/CC crossover size, including UCP AM header and payload. When CC is "
     "enabled, advertised max_short is capped below this value so UCP can "
     "select AM_ZCOPY at the crossover. The advertised AM_ZCOPY min_zcopy "
     "remains 0 because UCP proto-v2 rejects AM zcopy lanes with nonzero "
     "min_zcopy.",
     ucs_offsetof(uct_obmm_iface_config_t, cc_min_zcopy),
     UCS_CONFIG_TYPE_MEMUNITS},

    {"CC_CHUNK_SIZE", "4M",
     "Bytes per receiver-owned cacheable CC staging chunk. Must be aligned to "
     "CC_OWN_GRANULE; advertised as AM_ZCOPY max_zcopy. The 4 MiB default is "
     "the best measured high-concurrency point so far; larger chunks increase "
     "ownership pressure and regress 140-process OSU runs.",
     ucs_offsetof(uct_obmm_iface_config_t, cc_chunk_size),
     UCS_CONFIG_TYPE_MEMUNITS},

    {"CC_CHUNK_COUNT", "4",
     "Number of receiver-owned CC staging chunks per local iface/process slot.",
     ucs_offsetof(uct_obmm_iface_config_t, cc_chunk_count),
     UCS_CONFIG_TYPE_UINT},

    {"CC_MAX_IOV", "8",
     "Maximum iovcnt accepted by the CC staged AM_ZCOPY path.",
     ucs_offsetof(uct_obmm_iface_config_t, cc_max_iov),
     UCS_CONFIG_TYPE_UINT},

    {"CC_OWN_GRANULE", "2M",
     "Effective cacheable CC ownership granule. obmm_set_ownership() accepts "
     "page-aligned ranges, but target probing shows PMD/2 MiB-like cost and "
     "sharing behavior. CC chunks and slot bases must be aligned to this "
     "granule to avoid adjacent chunks sharing one ownership domain.",
     ucs_offsetof(uct_obmm_iface_config_t, cc_own_granule),
     UCS_CONFIG_TYPE_MEMUNITS},

     {NULL}
};


ucs_status_t
uct_obmm_iface_query_tl_devices(uct_md_h md,
                                uct_tl_device_resource_t **tl_devices_p,
                                unsigned *num_tl_devices_p)
{
    return uct_single_device_resource(md, UCT_OBMM_DEVICE_NAME,
                                      UCT_DEVICE_TYPE_SHM,
                                      UCS_SYS_DEVICE_ID_UNKNOWN, tl_devices_p,
                                      num_tl_devices_p);
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
                                   UCT_IFACE_FLAG_CB_SYNC          |
                                   UCT_IFACE_FLAG_INTER_NODE;
    attr->iface_addr_len         = sizeof(uct_obmm_iface_addr_t);
    attr->device_addr_len        = sizeof(uct_obmm_device_addr_t);
    attr->ep_addr_len            = 0;
    attr->max_conn_priv          = 0;

    /* UCT contract: max_short is total bytes the caller may pass as
     * (header + payload). obmm stores am_short inline in the shared FIFO
     * element starting at elem->header. */
    if (iface->cc.enabled && (iface->cc.min_zcopy > 0)) {
        /* UCP tests AM_SHORT before AM_ZCOPY. Keep the raw FIFO capacity for
         * validation, but cap the advertised short limit at the configured
         * NC/CC crossover so messages at CC_MIN_ZCOPY and above can select
         * the CC staged AM_ZCOPY path. */
        max_short = ucs_min(max_short, iface->cc.min_zcopy - 1);
    }

    attr->cap.am.max_short       = max_short;
    attr->cap.am.max_bcopy       = iface->bcopy_seg_size;
    attr->cap.am.min_zcopy       = 0;
    attr->cap.am.max_zcopy       = 0;
    attr->cap.am.max_iov         = 0;
    attr->cap.am.max_hdr         = 0;
    attr->cap.am.opt_zcopy_align = 1;
    attr->cap.am.align_mtu       = 1;

    if (iface->cc.enabled) {
        attr->cap.flags         |= UCT_IFACE_FLAG_AM_ZCOPY;
        attr->cap.am.min_zcopy  = 0;
        attr->cap.am.max_zcopy  = iface->cc.chunk_size;
        attr->cap.am.max_iov    = iface->cc.max_iov;
        attr->cap.am.max_hdr    = iface->cc.chunk_size;
    }

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
    case UCT_EP_OP_AM_ZCOPY:
        if (iface->cc.enabled) {
            send_pre_overhead = iface->config.cc_zcopy_overhead;
            recv_overhead     = iface->config.cc_zcopy_overhead;
            bandwidth         = iface->config.cc_bandwidth;
        }
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
    iaddr->slot_count       = UCT_OBMM_POOL_SLOT_COUNT;
    iaddr->short_lane_count = UCT_OBMM_SHORT_LANE_COUNT;
    iaddr->wire_format      = iface->cc.enabled ?
                              UCT_OBMM_WIRE_FORMAT_CCZCOPY :
                              UCT_OBMM_WIRE_FORMAT_INLINE32;
    iaddr->fifo_size        = iface->fifo_size;
    iaddr->fifo_elem_size   = iface->fifo_elem_size;
    iaddr->bcopy_seg_size   = iface->bcopy_seg_size;
    iaddr->cc_chunk_count   = iface->cc.enabled ?
                              (uint32_t)iface->cc.chunk_count : 0;
    iaddr->cc_chunk_size    = iface->cc.enabled ?
                              (uint32_t)iface->cc.chunk_size : 0;
    iaddr->cc_min_zcopy     = iface->cc.enabled ?
                              (uint32_t)iface->cc.min_zcopy : 0;
    iaddr->cc_own_granule   = iface->cc.enabled ?
                              (uint32_t)iface->cc.own_granule : 0;
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
    uint32_t                      expected_wire_format;

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
        uct_iface_fill_info_str_buf(params,
                                    "incompatible OBMM geometry "
                                    "(peer slots=%u lanes=%u wire=%u fifo=%u "
                                    "elem=%u seg=%u cc_count=%u cc_size=%u "
                                    "cc_min=%u cc_gran=%u, local slots=%u "
                                    "lanes=%u wire=%u fifo=%u elem=%u "
                                    "seg=%u cc_count=%u cc_size=%zu "
                                    "cc_min=%zu cc_gran=%zu)",
                                    iaddr->slot_count,
                                    iaddr->short_lane_count,
                                    iaddr->wire_format,
                                    iaddr->fifo_size, iaddr->fifo_elem_size,
                                    iaddr->bcopy_seg_size,
                                    iaddr->cc_chunk_count,
                                    iaddr->cc_chunk_size,
                                    iaddr->cc_min_zcopy,
                                    iaddr->cc_own_granule,
                                    UCT_OBMM_POOL_SLOT_COUNT,
                                    UCT_OBMM_SHORT_LANE_COUNT,
                                    expected_wire_format,
                                    iface->fifo_size, iface->fifo_elem_size,
                                    iface->bcopy_seg_size,
                                    iface->cc.enabled ?
                                    iface->cc.chunk_count : 0,
                                    iface->cc.enabled ?
                                    iface->cc.chunk_size : 0,
                                    iface->cc.enabled ?
                                    iface->cc.min_zcopy : 0,
                                    iface->cc.enabled ?
                                    iface->cc.own_granule : 0);
        return 0;
    }

    eid.hi = daddr->exporter_deid_hi;
    eid.lo = daddr->exporter_deid_lo;

    export_r = uct_obmm_md_export_region(md, UCT_OBMM_REGION_KIND_NC);
    if ((export_r != NULL) &&
        (export_r->info.exporter_dcna == daddr->exporter_dcna) &&
        (export_r->info.exporter_deid.hi == eid.hi) &&
        (export_r->info.exporter_deid.lo == eid.lo)) {
        goto check_cc;
    }

    if (uct_obmm_md_find_import_region(md, UCT_OBMM_REGION_KIND_NC,
                                       daddr->exporter_dcna, &eid) != NULL) {
        goto check_cc;
    }

    uct_iface_fill_info_str_buf(params,
                                "no mapped region for peer dcna=0x%lx "
                                "deid=0x%lx:0x%lx",
                                (unsigned long)daddr->exporter_dcna,
                                (unsigned long)eid.hi, (unsigned long)eid.lo);
    return 0;

check_cc:
    if (iface->cc.enabled &&
        (uct_obmm_md_find_region(md, UCT_OBMM_REGION_KIND_CC,
                                 daddr->exporter_dcna, &eid) == NULL)) {
        uct_iface_fill_info_str_buf(params,
                                    "no mapped CC region for peer "
                                    "dcna=0x%lx deid=0x%lx:0x%lx",
                                    (unsigned long)daddr->exporter_dcna,
                                    (unsigned long)eid.hi,
                                    (unsigned long)eid.lo);
        return 0;
    }

    return uct_iface_scope_is_reachable(tl_iface, params);
}


static unsigned uct_obmm_iface_progress(uct_iface_h tl_iface)
{
    uct_obmm_iface_t        *iface = ucs_derived_of(tl_iface, uct_obmm_iface_t);
    unsigned                 polled = 0;
    unsigned                 pending_progress = 0;
    uct_obmm_fifo_element_t *elem;
    uint8_t                  flags;
    uint8_t                  expected_owner;
    size_t                   max_poll = iface->fifo_poll_count;
    unsigned                 pending_ack_progress;
    unsigned                 pending_ready_progress;

    pending_ack_progress = uct_obmm_iface_progress_cc_acks(iface);
    pending_ready_progress = uct_obmm_iface_progress_cc_ready(iface);

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

        ucs_memory_bus_load_fence();

        if (elem->generation != iface->generation) {
            /* Stale write from a previous slot owner (we were torn down and
             * re-allocated this slot). Drop silently. */
            ucs_trace_data("obmm: drop stale elem (gen=%u expected=%u) "
                           "at idx=%lu", elem->generation, iface->generation,
                           (unsigned long)iface->read_index);
        } else if (flags & UCT_OBMM_FIFO_ELEM_FLAG_CC_DATA_READY) {
            if (ucs_unlikely(elem->length !=
                             sizeof(uct_obmm_cc_data_ready_t))) {
                ucs_error("obmm: invalid CC_DATA_READY length %u at idx=%lu",
                          elem->length, (unsigned long)iface->read_index);
            } else {
                const uct_obmm_cc_data_ready_t *ready =
                    (const uct_obmm_cc_data_ready_t*)
                    ((char*)elem + ucs_offsetof(uct_obmm_fifo_element_t,
                                                header));
                uct_obmm_iface_handle_cc_data_ready(iface, elem->am_id,
                                                    ready);
            }
        } else if (flags & UCT_OBMM_FIFO_ELEM_FLAG_CC_ACK) {
            if (ucs_unlikely(elem->length != sizeof(uct_obmm_cc_ack_t))) {
                ucs_error("obmm: invalid CC_ACK length %u at idx=%lu",
                          elem->length, (unsigned long)iface->read_index);
            } else {
                const uct_obmm_cc_ack_t *ack =
                    (const uct_obmm_cc_ack_t*)
                    ((char*)elem + ucs_offsetof(uct_obmm_fifo_element_t,
                                                header));
                uct_obmm_iface_handle_cc_ack(iface, ack);
            }
        } else if (flags & UCT_OBMM_FIFO_ELEM_FLAG_BCOPY) {
            /* am_bcopy: payload is in the paired desc[N], not in the FIFO
             * element body. The bus_load_fence above orders this load
             * with respect to the sender's bus_store_fence + flag write. */
            if (ucs_unlikely(elem->length > iface->bcopy_seg_size)) {
                ucs_error("obmm: invalid bcopy length %u at idx=%lu "
                          "(seg_size=%u gen=%u expected=%u)", elem->length,
                          (unsigned long)iface->read_index,
                          iface->bcopy_seg_size, elem->generation,
                          iface->generation);
            } else {
                void *desc = uct_obmm_slot_desc(iface->recv_descs,
                                                iface->read_index,
                                                iface->fifo_mask,
                                                iface->bcopy_seg_size);
                uct_iface_invoke_am(&iface->super, elem->am_id,
                                    desc, elem->length, 0);
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
                void *short_data = (char*)elem +
                                   ucs_offsetof(uct_obmm_fifo_element_t,
                                                header);
                uct_iface_invoke_am(&iface->super, elem->am_id,
                                    short_data, elem->length, 0);
            }
        }

        iface->read_index++;
        polled++;
    }

    uct_obmm_iface_fifo_window_adjust(iface, polled);

    if (polled > 0) {
        /* Full bus fence: orders the AM handler's LOADS from desc[]/elem
         * payload BEFORE the STORE that publishes the new tail. A plain
         * bus_store_fence (e.g. dmb oshst on aarch64) only orders
         * store→store, which would let a sender observe the advanced
         * tail and overwrite desc[N] while we still have outstanding
         * loads in flight. See obmm_fifo.h:uct_obmm_bus_full_fence. */
        uct_obmm_bus_full_fence();
        iface->recv_ctl->tail = iface->read_index;
    }

    /* Drain any UCP requests waiting on TX backpressure. The peer-side
     * tail advance we just published may also have freed slots that *our*
     * pending eps have been waiting for; dispatch with a fresh head/tail
     * snapshot so queued retries see the latest state. */
    ucs_arbiter_dispatch(&iface->arbiter, 1, uct_obmm_ep_process_pending,
                         &pending_progress);

    pending_ready_progress += uct_obmm_iface_progress_cc_ready(iface);
    pending_ack_progress += uct_obmm_iface_progress_cc_acks(iface);

    return polled + pending_progress + pending_ack_progress +
           pending_ready_progress;
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
    uct_obmm_iface_t *iface = ucs_derived_of(tl_iface, uct_obmm_iface_t);

    (void)flags;

    if (iface->cc.outstanding == 0) {
        UCT_TL_IFACE_STAT_FLUSH(&iface->super);
        return UCS_OK;
    }

    if (comp != NULL) {
        if (iface->cc.flush_comp != NULL) {
            return UCS_ERR_NO_RESOURCE;
        }
        iface->cc.flush_comp = comp;
    }

    UCT_TL_IFACE_STAT_FLUSH_WAIT(&iface->super);
    return UCS_INPROGRESS;
}


static ucs_status_t uct_obmm_ep_fence(uct_ep_h tl_ep, unsigned flags)
{
    (void)flags;
    ucs_memory_cpu_fence();
    UCT_TL_EP_STAT_FENCE(ucs_derived_of(tl_ep, uct_base_ep_t));
    return UCS_OK;
}


static ucs_status_t
uct_obmm_iface_validate_cc_config(uct_obmm_iface_config_t *config,
                                  uct_obmm_region_t *nc_region,
                                  uct_obmm_region_t *cc_region,
                                  size_t *slot_stride_p)
{
    size_t page_size = ucs_get_page_size();
    size_t slot_stride;
    size_t required;

    *slot_stride_p = 0;

    if ((cc_region == NULL) || (config->cc_chunk_count == 0) ||
        (config->cc_max_iov == 0)) {
        return UCS_OK;
    }

    if ((cc_region->info.exporter_dcna != nc_region->info.exporter_dcna) ||
        (cc_region->info.exporter_deid.hi != nc_region->info.exporter_deid.hi) ||
        (cc_region->info.exporter_deid.lo != nc_region->info.exporter_deid.lo)) {
        ucs_error("obmm: local CC export identity does not match local NC "
                  "export identity; cannot key CC by NC device address");
        return UCS_ERR_INVALID_PARAM;
    }

    if (config->cc_chunk_count > UCT_OBMM_IFACE_CC_CHUNK_COUNT_MAX) {
        ucs_error("obmm: CC_CHUNK_COUNT (%u) exceeds max %u",
                  config->cc_chunk_count, UCT_OBMM_IFACE_CC_CHUNK_COUNT_MAX);
        return UCS_ERR_INVALID_PARAM;
    }

    if ((config->cc_chunk_size == 0) ||
        ((config->cc_chunk_size % page_size) != 0)) {
        ucs_error("obmm: CC_CHUNK_SIZE (%zu) must be a nonzero multiple of "
                  "page size %zu", config->cc_chunk_size, page_size);
        return UCS_ERR_INVALID_PARAM;
    }

    if ((config->cc_own_granule < page_size) ||
        ((config->cc_own_granule % page_size) != 0) ||
        !ucs_is_pow2(config->cc_own_granule)) {
        ucs_error("obmm: CC_OWN_GRANULE (%zu) must be a power-of-two "
                  "multiple of page size %zu", config->cc_own_granule,
                  page_size);
        return UCS_ERR_INVALID_PARAM;
    }

    if ((config->cc_chunk_size % config->cc_own_granule) != 0) {
        ucs_error("obmm: CC_CHUNK_SIZE (%zu) must be a multiple of "
                  "CC_OWN_GRANULE (%zu)", config->cc_chunk_size,
                  config->cc_own_granule);
        return UCS_ERR_INVALID_PARAM;
    }

    if ((config->cc_min_zcopy == 0) ||
        (config->cc_min_zcopy > config->cc_chunk_size)) {
        ucs_error("obmm: CC_MIN_ZCOPY (%zu) must be >0 and <= "
                  "CC_CHUNK_SIZE (%zu)", config->cc_min_zcopy,
                  config->cc_chunk_size);
        return UCS_ERR_INVALID_PARAM;
    }

    if ((config->cc_chunk_size > UINT32_MAX) ||
        (config->cc_min_zcopy > UINT32_MAX) ||
        (config->cc_own_granule > UINT32_MAX)) {
        ucs_error("obmm: CC zcopy sizes must fit in 32-bit wire fields "
                  "(chunk=%zu min=%zu granule=%zu)",
                  config->cc_chunk_size, config->cc_min_zcopy,
                  config->cc_own_granule);
        return UCS_ERR_INVALID_PARAM;
    }

    if (((uintptr_t)cc_region->base % config->cc_own_granule) != 0) {
        ucs_error("obmm: CC mapping base %p is not aligned to "
                  "CC_OWN_GRANULE (%zu)", cc_region->base,
                  config->cc_own_granule);
        return UCS_ERR_INVALID_PARAM;
    }

    if (config->cc_chunk_size >
        (SIZE_MAX / (size_t)config->cc_chunk_count)) {
        return UCS_ERR_INVALID_PARAM;
    }

    slot_stride = config->cc_chunk_size * config->cc_chunk_count;
    if (slot_stride > (SIZE_MAX / UCT_OBMM_POOL_SLOT_COUNT)) {
        return UCS_ERR_INVALID_PARAM;
    }

    required = slot_stride * UCT_OBMM_POOL_SLOT_COUNT;
    if (required > cc_region->length) {
        ucs_error("obmm: CC geometry does not fit in region: chunk_size=%zu "
                  "chunk_count=%u slot_count=%u required=%zu region=%zu",
                  config->cc_chunk_size, config->cc_chunk_count,
                  UCT_OBMM_POOL_SLOT_COUNT, required, cc_region->length);
        return UCS_ERR_INVALID_PARAM;
    }

    *slot_stride_p = slot_stride;
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
    uct_obmm_region_t       *cc_region;
    size_t                   stride;
    size_t                   cc_slot_stride = 0;
    size_t                   required;
    ucs_status_t             status;

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
    if ((config->super.bandwidth <= 1.0) ||
        (config->cc_bandwidth <= 1.0)) {
        ucs_error("obmm: BW and CC_BW must be positive");
        return UCS_ERR_INVALID_PARAM;
    }
    if ((config->short_overhead < 0.0) ||
        (config->bcopy_overhead < 0.0) ||
        (config->cc_zcopy_overhead < 0.0)) {
        ucs_error("obmm: performance overhead estimates must be non-negative");
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
    if (config->bcopy_seg_size < UCT_OBMM_MIN_BCOPY_SEG_SIZE) {
        ucs_error("obmm: BCOPY_SEG_SIZE (%u) is too small; UCP requires "
                  "AM bcopy transports to provide at least %u bytes",
                  config->bcopy_seg_size, UCT_OBMM_MIN_BCOPY_SEG_SIZE);
        return UCS_ERR_INVALID_PARAM;
    }

    region = uct_obmm_md_export_region(md, UCT_OBMM_REGION_KIND_NC);
    if (region == NULL) {
        ucs_error("obmm: cannot create iface; this MD has no local export "
                  "NC region");
        return UCS_ERR_NO_DEVICE;
    }

    cc_region = uct_obmm_md_export_region(md, UCT_OBMM_REGION_KIND_CC);
    status = uct_obmm_iface_validate_cc_config(config, region, cc_region,
                                               &cc_slot_stride);
    if (status != UCS_OK) {
        return status;
    }

    /* Compute slot stride as size_t, then validate it fits in u32 (the
     * pool header field is u32) AND that the total region budget covers
     * slot_count slots. Failing here is preferred over silently capping
     * BCOPY_SEG_SIZE — UCP would happily make protocol decisions based
     * on a quietly reduced max_bcopy. */
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
                  "Reduce UCX_OBMM_BCOPY_SEG_SIZE, UCX_OBMM_FIFO_SIZE, or "
                  "UCX_OBMM_FIFO_ELEM_SIZE.",
                  config->fifo_size, config->fifo_elem_size,
                  config->bcopy_seg_size, stride,
                  UCT_OBMM_POOL_SLOT_COUNT, required, region->length);
        return UCS_ERR_INVALID_PARAM;
    }

    UCT_CHECK_PARAM(params->field_mask & UCT_IFACE_PARAM_FIELD_OPEN_MODE,
                    "UCT_IFACE_PARAM_FIELD_OPEN_MODE is not defined");
    if (!(params->open_mode & UCT_IFACE_OPEN_MODE_DEVICE)) {
        ucs_error("only UCT_IFACE_OPEN_MODE_DEVICE is supported");
        return UCS_ERR_UNSUPPORTED;
    }

    UCS_CLASS_CALL_SUPER_INIT(uct_base_iface_t, &uct_obmm_iface_ops,
                              &uct_obmm_iface_internal_ops, tl_md, worker,
                              params, &config->super.super
                              UCS_STATS_ARG((params->field_mask &
                                             UCT_IFACE_PARAM_FIELD_STATS_ROOT) ?
                                            params->stats_root : NULL)
                              UCS_STATS_ARG(params->mode.device.dev_name));

    self->region                   = region;
    self->config.bandwidth         = config->super.bandwidth;
    self->config.short_overhead    = config->short_overhead;
    self->config.bcopy_overhead    = config->bcopy_overhead;
    self->config.cc_bandwidth      = config->cc_bandwidth;
    self->config.cc_zcopy_overhead = config->cc_zcopy_overhead;
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
    self->cc.enabled      = 0;
    self->cc.region       = NULL;
    self->cc.slot_base    = NULL;
    self->cc.slot_stride  = 0;
    self->cc.chunk_size   = 0;
    self->cc.min_zcopy    = 0;
    self->cc.own_granule  = 0;
    self->cc.chunk_count  = 0;
    self->cc.max_iov      = 0;
    self->cc.next_seq     = 0;
    self->cc.rx_tail      = 0;
    self->cc.rx_done_mask = 0;
    self->cc.outstanding  = 0;
    self->cc.flush_comp   = NULL;
    self->cc.tx_slots     = NULL;
    self->cc.pending_acks = NULL;

    status = uct_obmm_pool_attach(region->base, region->length,
                                  UCT_OBMM_POOL_SLOT_COUNT,
                                  (uint32_t)stride, &self->pool);
    if (status != UCS_OK) {
        ucs_error("obmm: pool attach failed: %s", ucs_status_string(status));
        return status;
    }

    status = uct_obmm_pool_alloc_slot(&self->pool, &self->slot_index,
                                      &self->recv_slot, &self->generation);
    if (status != UCS_OK) {
        ucs_error("obmm: failed to allocate FIFO slot: %s",
                  ucs_status_string(status));
        return status;
    }

    self->recv_ctl   = uct_obmm_slot_ctl(self->recv_slot);
    self->recv_elems = uct_obmm_slot_elems(self->recv_slot);
    self->recv_descs = uct_obmm_slot_descs(self->recv_slot, self->fifo_size,
                                           self->fifo_elem_size);

    if ((cc_region != NULL) && (config->cc_chunk_count > 0) &&
        (config->cc_max_iov > 0)) {
        self->cc.region       = cc_region;
        self->cc.slot_stride  = cc_slot_stride;
        self->cc.chunk_size   = config->cc_chunk_size;
        self->cc.min_zcopy    = config->cc_min_zcopy;
        self->cc.own_granule  = config->cc_own_granule;
        self->cc.chunk_count  = config->cc_chunk_count;
        self->cc.max_iov      = config->cc_max_iov;
        self->cc.slot_base    = UCS_PTR_BYTE_OFFSET(
                                cc_region->base,
                                (size_t)self->slot_index * cc_slot_stride);
        self->cc.next_seq     = 1;
        self->cc.rx_tail      = self->recv_ctl->cc_tail;
        self->cc.rx_done_mask = 0;
        self->cc.outstanding  = 0;
        self->cc.flush_comp   = NULL;
        self->cc.tx_slots     = NULL;

        status = uct_obmm_region_set_ownership(cc_region, self->cc.slot_base,
                                               self->cc.slot_stride,
                                               PROT_NONE);
        if (status != UCS_OK) {
            goto err_free_slot;
        }

        self->cc.enabled = 1;
    }

    ucs_arbiter_init(&self->arbiter);

    /* recv_slot was zeroed by pool_alloc_slot, so head/tail/all element
     * flags are zero. read_index starts at 0, expected owner bit on the
     * first lap is 1; uninitialized zero correctly reads as "not yet
     * written". */

    ucs_debug("obmm: iface %p attached to region %p slot=%u gen=%u "
              "fifo_size=%u elem_size=%u seg_size=%u stride=%zu "
              "cc_enabled=%d cc_chunk=%zu cc_count=%u cc_gran=%zu",
              self, region->base, self->slot_index, self->generation,
              self->fifo_size, self->fifo_elem_size, self->bcopy_seg_size,
              stride, self->cc.enabled, self->cc.chunk_size,
              self->cc.chunk_count, self->cc.own_granule);
    return UCS_OK;

err_free_slot:
    uct_obmm_pool_free_slot(&self->pool, self->slot_index);
    return status;
}


static UCS_CLASS_CLEANUP_FUNC(uct_obmm_iface_t)
{
    uct_base_iface_progress_disable(&self->super.super,
                                    UCT_PROGRESS_SEND | UCT_PROGRESS_RECV);
    uct_obmm_iface_cleanup_cc(self);
    if ((self->pool.hdr != NULL) &&
        uct_obmm_pool_free_slot(&self->pool, self->slot_index)) {
        uct_obmm_pool_reset(&self->pool);
    }
    /* All eps were destroyed before iface cleanup (UCX framework
     * contract; mm relies on the same), so the arbiter is empty. */
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
    .ep_am_zcopy              = uct_obmm_ep_am_zcopy,
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
    .iface_estimate_perf   = uct_obmm_iface_estimate_perf,
    .iface_vfs_refresh     = (uct_iface_vfs_refresh_func_t)ucs_empty_function,
    .ep_query              = (uct_ep_query_func_t)ucs_empty_function_return_unsupported,
    .ep_invalidate         = (uct_ep_invalidate_func_t)ucs_empty_function_return_unsupported,
    .ep_connect_to_ep_v2   = ucs_empty_function_return_unsupported,
    .iface_is_reachable_v2 = uct_obmm_iface_is_reachable_v2,
    .ep_is_connected       = uct_obmm_ep_is_connected
};


UCT_TL_DEFINE_ENTRY(&uct_obmm_component, obmm, uct_obmm_iface_query_tl_devices,
                    uct_obmm_iface_t, "OBMM_", uct_obmm_iface_config_table,
                    uct_obmm_iface_config_t);

UCT_SINGLE_TL_INIT(&uct_obmm_component, obmm,,,)
