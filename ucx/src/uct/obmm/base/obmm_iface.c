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
#include "obmm_block.h"
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
#include <ucs/vfs/base/vfs_cb.h>
#include <ucs/vfs/base/vfs_obj.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


static uct_iface_ops_t          uct_obmm_iface_ops;
static uct_iface_internal_ops_t uct_obmm_iface_internal_ops;

#define UCT_OBMM_DEVICE_NAME "memory"
#define UCT_OBMM_MIN_BCOPY_SEG_SIZE 64u

enum {
    UCT_OBMM_VFS_RX_HEAD,
    UCT_OBMM_VFS_RX_TAIL
};


static UCS_F_ALWAYS_INLINE void
uct_obmm_iface_fifo_window_adjust(uct_obmm_iface_t *iface,
                                  uct_obmm_iface_rx_t *rx,
                                  unsigned rx_count)
{
    if (rx_count < rx->fifo_poll_count) {
        rx->fifo_poll_count = ucs_max(rx->fifo_poll_count /
                                      UCT_OBMM_IFACE_FIFO_MD_FACTOR,
                                      iface->fifo_min_poll);
        rx->fifo_prev_wnd_cons = 0;
        return;
    }

    ucs_assert(rx_count == rx->fifo_poll_count);
    if (rx->fifo_prev_wnd_cons) {
        rx->fifo_poll_count = ucs_min(rx->fifo_poll_count +
                                      UCT_OBMM_IFACE_FIFO_AI_VALUE,
                                      iface->fifo_max_poll);
    } else {
        rx->fifo_prev_wnd_cons = 1;
    }
}


ucs_config_field_t uct_obmm_iface_config_table[] = {
    {"", "", NULL, ucs_offsetof(uct_obmm_iface_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_iface_config_table)},

    {"BW", "3400MBs",
     "Effective transport bandwidth used for UCP lane/protocol cost "
     "modeling. This is not a required knob: if the user does not set "
     "UCX_OBMM_BW, obmm uses this sustained NC default.",
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

    {"FIFO_SIZE", UCS_PP_MAKE_STRING(UCT_OBMM_IFACE_FIFO_SIZE_DEFAULT),
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

    {"FIFO_MIN_POLL",
     UCS_PP_MAKE_STRING(UCT_OBMM_IFACE_FIFO_MIN_POLL_DEFAULT),
     "Minimal receive completions to drain in one progress() call. The loop "
     "still exits immediately when the next FIFO element is not published.",
     ucs_offsetof(uct_obmm_iface_config_t, fifo_min_poll),
      UCS_CONFIG_TYPE_ULUNITS},

    {"FIFO_MAX_POLL",
     UCS_PP_MAKE_STRING(UCT_OBMM_IFACE_FIFO_MAX_POLL_DEFAULT),
     "Maximal receive completions to drain in one progress() call. Adaptive "
     "polling grows toward the default half-ring batch under sustained "
     "receive pressure.",
     ucs_offsetof(uct_obmm_iface_config_t, fifo_max_poll),
        UCS_CONFIG_TYPE_ULUNITS},

    {"PENDING_QUOTA", "1",
     "How many pending send retries may be dispatched during iface progress. "
     "Defaults to the latency-friendly single-dispatch behavior.",
     ucs_offsetof(uct_obmm_iface_config_t, pending_quota),
     UCS_CONFIG_TYPE_UINT},

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

    /* UCP worker-address v1 reserves flag bits in these length fields. */
    UCS_STATIC_ASSERT(sizeof(uct_obmm_device_addr_t) <= 31u);
    uct_base_iface_query(&iface->super, attr);
    attr->cap.flags              = UCT_IFACE_FLAG_AM_SHORT         |
                                   UCT_IFACE_FLAG_AM_BCOPY         |
                                   UCT_IFACE_FLAG_PENDING          |
                                   UCT_IFACE_FLAG_CONNECT_TO_IFACE |
                                   UCT_IFACE_FLAG_CB_SYNC;
    if (iface->rx.active) {
        attr->cap.flags |= UCT_IFACE_FLAG_INTER_NODE;
    }
    attr->iface_addr_len         = 0;
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
    attr->cap.am.max_iov         = SIZE_MAX;
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
    uct_obmm_region_t      *region;

    memset(daddr, 0, sizeof(*daddr));
    region = iface->rx.region;
    ucs_assert(region != NULL);
    daddr->primary.exporter_dcna = region->info.exporter_dcna;
    daddr->primary.exporter_deid = region->info.exporter_deid;
    daddr->primary.region_id     = region->info.region_id;
    return UCS_OK;
}


static int
uct_obmm_iface_region_matches_addr(const uct_obmm_region_t *region,
                                   const uct_obmm_region_addr_t *addr)
{
    return (region->info.exporter_dcna == addr->exporter_dcna) &&
           (region->info.exporter_deid == addr->exporter_deid) &&
           (region->info.region_id == addr->region_id);
}

ucs_status_t
uct_obmm_iface_resolve_peer(uct_obmm_iface_t *iface,
                            const uct_obmm_device_addr_t *daddr,
                            uct_obmm_dev_info_t *info_p,
                            int *use_rx_region_p)
{
    uct_obmm_md_t  *md = ucs_derived_of(iface->super.md, uct_obmm_md_t);
    ucs_status_t    status;

    if (!iface->rx.active) {
        return UCS_ERR_UNREACHABLE;
    }

    if ((iface->rx.region != NULL) &&
        uct_obmm_iface_region_matches_addr(iface->rx.region,
                                           &daddr->primary)) {
        *info_p            = iface->rx.region->info;
        *use_rx_region_p   = 1;
        return UCS_OK;
    }

    status = uct_obmm_md_find_device(md, daddr->primary.exporter_dcna,
                                     daddr->primary.exporter_deid,
                                     daddr->primary.region_id, info_p);
    if (status != UCS_OK) {
        return status;
    }

    *use_rx_region_p   = 0;
    return UCS_OK;
}


static int
uct_obmm_iface_is_reachable_v2(const uct_iface_h tl_iface,
                               const uct_iface_is_reachable_params_t *params)
{
    uct_obmm_iface_t             *iface = ucs_derived_of(tl_iface,
                                                         uct_obmm_iface_t);
    const uct_obmm_device_addr_t *daddr;
    uct_obmm_dev_info_t           peer_info;
    int                           use_rx_region;

    if (!uct_iface_is_reachable_params_valid(
                params, UCT_IFACE_IS_REACHABLE_FIELD_DEVICE_ADDR)) {
        return 0;
    }

    daddr = (const uct_obmm_device_addr_t*)params->device_addr;
    if (daddr == NULL) {
        uct_iface_fill_info_str_buf(params, "missing device address");
        return 0;
    }

    if (uct_obmm_iface_resolve_peer(iface, daddr, &peer_info,
                                    &use_rx_region) == UCS_OK) {
        return uct_iface_scope_is_reachable(tl_iface, params);
    }

    uct_iface_fill_info_str_buf(params,
                                "no compatible mapped region for peer primary "
                                "dcna=0x%lx deid=0x%x region_id=0x%x",
                                (unsigned long)daddr->primary.exporter_dcna,
                                daddr->primary.exporter_deid,
                                daddr->primary.region_id);
    return 0;
}


static UCS_F_ALWAYS_INLINE void
uct_obmm_iface_load_fence(void)
{
    ucs_memory_bus_load_fence();
}


static UCS_F_ALWAYS_INLINE void
uct_obmm_iface_full_fence(void)
{
    uct_obmm_bus_full_fence();
}


static unsigned
uct_obmm_iface_progress_rx(uct_obmm_iface_t *iface,
                           uct_obmm_iface_rx_t *rx)
{
    unsigned                 polled = 0;
    uct_obmm_fifo_element_t *elem;
    uint8_t                  flags;
    uint8_t                  expected_owner;
    size_t                   max_poll = rx->fifo_poll_count;

    while (polled < max_poll) {
        elem = uct_obmm_fifo_elem_at(rx->recv_elems, rx->read_index,
                                     iface->fifo_mask,
                                     iface->fifo_elem_size);

        /* Owner bit alternates each lap of the ring; pass 0 expects 1, pass
         * 1 expects 0, etc. Combined with zero-fill on block claim, this
         * means an unwritten element reads as flags==0 and is correctly
         * skipped on the very first lap. */
        expected_owner = (rx->read_index & iface->fifo_size) ?
                         0u : UCT_OBMM_FIFO_ELEM_FLAG_OWNER;

        flags = elem->flags;
        if ((flags & UCT_OBMM_FIFO_ELEM_FLAG_OWNER) != expected_owner) {
            break;
        }

        uct_obmm_iface_load_fence();

        if (flags & UCT_OBMM_FIFO_ELEM_FLAG_BCOPY) {
            /* am_bcopy: payload starts at the FIFO element's 64-byte-aligned
             * bcopy offset. The load fence above orders this load with
             * respect to the sender's matching store fence + flag write. */
            if (ucs_unlikely(elem->length > iface->bcopy_seg_size)) {
                ucs_error("obmm: invalid bcopy length %u at idx=%lu "
                          "(seg_size=%u)", elem->length,
                          (unsigned long)rx->read_index,
                          iface->bcopy_seg_size);
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
                          "(max_short=%u)",
                          elem->length, (unsigned long)rx->read_index,
                          uct_obmm_fifo_max_short(iface->fifo_elem_size));
            } else {
                uct_iface_invoke_am(&iface->super, elem->am_id,
                                    uct_obmm_fifo_elem_short_data(elem),
                                    elem->length, 0);
            }
        }

        rx->read_index++;
        polled++;
    }

    uct_obmm_iface_fifo_window_adjust(iface, rx, polled);

    if (polled > 0) {
        /* Full release fence: orders the AM handler's LOADS from FIFO payload
         * BEFORE the STORE that publishes the new tail. A plain store fence
         * would let a sender observe the advanced tail and overwrite the FIFO
         * entry while we still have outstanding loads in flight. */
        uct_obmm_iface_full_fence();
        rx->recv_ctl->tail = rx->read_index;
    }

    return polled;
}


static unsigned uct_obmm_iface_progress_pending(uct_obmm_iface_t *iface)
{
    unsigned pending_progress = 0;

    ucs_arbiter_dispatch(&iface->arbiter, 1, uct_obmm_ep_process_pending,
                         &pending_progress);
    return pending_progress;
}


static unsigned uct_obmm_iface_progress(uct_iface_h tl_iface)
{
    uct_obmm_iface_t *iface = ucs_derived_of(tl_iface, uct_obmm_iface_t);
    unsigned          total = 0;

    if (iface->rx.active) {
        total += uct_obmm_iface_progress_rx(iface, &iface->rx);
    }
    total += uct_obmm_iface_progress_pending(iface);
    return total;
}


static ucs_status_t uct_obmm_iface_fence(uct_iface_h tl_iface, unsigned flags)
{
    (void)flags;
    uct_obmm_iface_full_fence();
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


static void uct_obmm_vfs_read_u64(void *obj, ucs_string_buffer_t *strb,
                                  void *arg_ptr, uint64_t arg_u64)
{
    (void)obj;
    (void)arg_u64;

    ucs_string_buffer_appendf(strb, "%" PRIu64 "\n",
                              *(const uint64_t*)arg_ptr);
}


static void uct_obmm_vfs_read_rx_ctl(void *obj, ucs_string_buffer_t *strb,
                                     void *arg_ptr, uint64_t arg_u64)
{
    const uct_obmm_fifo_ctl_t *ctl = arg_ptr;
    uint64_t                   value;

    (void)obj;

    value = (arg_u64 == UCT_OBMM_VFS_RX_HEAD) ? ctl->head : ctl->tail;
    ucs_string_buffer_appendf(strb, "%" PRIu64 "\n", value);
}


static void uct_obmm_iface_vfs_refresh(uct_iface_h tl_iface)
{
    uct_obmm_iface_t    *iface = ucs_derived_of(tl_iface, uct_obmm_iface_t);
    uct_obmm_iface_rx_t *rx    = &iface->rx;

    ucs_vfs_obj_add_ro_file(iface, ucs_vfs_show_primitive,
                            &iface->fifo_size, UCS_VFS_TYPE_U32,
                            "fifo_size");
    ucs_vfs_obj_add_ro_file(iface, ucs_vfs_show_primitive,
                            &iface->fifo_elem_size, UCS_VFS_TYPE_U32,
                            "fifo_elem_size");
    ucs_vfs_obj_add_ro_file(iface, ucs_vfs_show_primitive,
                            &iface->bcopy_seg_size, UCS_VFS_TYPE_U32,
                            "bcopy_seg_size");
    ucs_vfs_obj_add_ro_file(iface, ucs_vfs_show_primitive,
                            &iface->pending_quota, UCS_VFS_TYPE_U32,
                            "pending_quota");

    if (rx->active) {
        ucs_vfs_obj_add_ro_file(iface, ucs_vfs_show_primitive,
                                &rx->fifo_poll_count, UCS_VFS_TYPE_SIZET,
                                "rx/fifo_poll_count");
        ucs_vfs_obj_add_ro_file(iface, uct_obmm_vfs_read_u64,
                                &rx->read_index, 0, "rx/read_index");
        ucs_vfs_obj_add_ro_file(iface, uct_obmm_vfs_read_rx_ctl, rx->recv_ctl,
                                UCT_OBMM_VFS_RX_HEAD, "rx/head");
        ucs_vfs_obj_add_ro_file(iface, uct_obmm_vfs_read_rx_ctl, rx->recv_ctl,
                                UCT_OBMM_VFS_RX_TAIL, "rx/tail");
        ucs_vfs_obj_add_ro_file(iface, ucs_vfs_show_primitive,
                                &rx->block.fifo_stride, UCS_VFS_TYPE_SIZET,
                                "rx/block/fifo_stride");
        ucs_vfs_obj_add_ro_file(iface, ucs_vfs_show_primitive,
                                &rx->block.fifo_offset, UCS_VFS_TYPE_SIZET,
                                "rx/block/fifo_offset");
        ucs_vfs_obj_add_ro_file(iface, ucs_vfs_show_primitive,
                                &rx->block.length, UCS_VFS_TYPE_SIZET,
                                "rx/block/length");
    }
}


static ucs_status_t uct_obmm_ep_fence(uct_ep_h tl_ep, unsigned flags)
{
    uct_obmm_ep_t *ep = ucs_derived_of(tl_ep, uct_obmm_ep_t);

    (void)flags;
    uct_obmm_iface_full_fence();
    UCT_TL_EP_STAT_FENCE(&ep->super);
    return UCS_OK;
}


static size_t
uct_obmm_iface_block_fifo_offset(const uct_obmm_region_t *region,
                                 size_t fifo_stride)
{
    return uct_obmm_block_colored_fifo_offset(fifo_stride, region->length,
                                              region->info.region_id);
}


static ucs_status_t
uct_obmm_iface_try_attach_rx(uct_obmm_iface_t *iface,
                             uct_obmm_region_t *region, size_t fifo_stride)
{
    uct_obmm_iface_rx_t *rx = &iface->rx;
    size_t               fifo_offset;
    size_t               required;
    ucs_status_t         status;

    rx->region = region;
    fifo_offset = uct_obmm_iface_block_fifo_offset(region, fifo_stride);
    required    = uct_obmm_block_required_size(fifo_stride, fifo_offset);
    if (required > rx->region->length) {
        ucs_error("obmm: geometry does not fit in export memid=%" PRIu64
                  ": fifo_size=%u elem_size=%u seg_size=%u stride=%zu "
                  "fifo_offset=%zu required=%zu region=%zu. "
                  "Reduce UCX_OBMM_BCOPY_SEG_SIZE, UCX_OBMM_FIFO_SIZE, or "
                  "UCX_OBMM_FIFO_ELEM_SIZE.",
                  region->info.memid, iface->fifo_size, iface->fifo_elem_size,
                  iface->bcopy_seg_size, fifo_stride, fifo_offset, required,
                  region->length);
        rx->region = NULL;
        return UCS_ERR_INVALID_PARAM;
    }

    status = uct_obmm_block_attach(rx->region->base, rx->region->length,
                                   fifo_stride, fifo_offset, &rx->block);
    if (status != UCS_OK) {
        if (status != UCS_ERR_NO_RESOURCE) {
            ucs_error("obmm: FIFO block attach failed: %s",
                      ucs_status_string(status));
        }
        memset(&rx->block, 0, sizeof(rx->block));
        rx->region     = NULL;
        rx->recv_ctl   = NULL;
        rx->recv_elems = NULL;
        return status;
    }

    rx->recv_ctl            = rx->block.ctl;
    rx->recv_elems          = rx->block.elems;
    rx->fifo_poll_count     = iface->fifo_min_poll;
    rx->fifo_prev_wnd_cons  = 0;
    rx->read_index          = 0;
    rx->active              = 1;

    ucs_debug("obmm: iface %p claimed export memid=%" PRIu64
              " region_id=0x%x base=%p fifo_size=%u elem_size=%u "
              "seg_size=%u stride=%zu fifo_offset=%zu",
              iface, rx->region->info.memid, rx->region->info.region_id,
              rx->region->base, iface->fifo_size, iface->fifo_elem_size,
              iface->bcopy_seg_size, fifo_stride, fifo_offset);
    return UCS_OK;
}


static ucs_status_t
uct_obmm_iface_attach_rx(uct_obmm_iface_t *iface, uct_obmm_md_t *md,
                         size_t fifo_stride)
{
    uct_obmm_iface_rx_t *rx = &iface->rx;
    uct_obmm_dev_info_t *devices = NULL;
    ucs_status_t         status;
    unsigned             i, num_devices = 0;
    unsigned             num_exports = 0;

    status = uct_obmm_md_discover_devices(md, &devices, &num_devices);
    if (status != UCS_OK) {
        ucs_error("obmm: failed to discover shmdevs for RX export: %s",
                  ucs_status_string(status));
        return status;
    }

    for (i = 0; i < num_devices; ++i) {
        if (devices[i].type != UCT_OBMM_DEV_EXPORT) {
            continue;
        }
        ++num_exports;

        rx->region_storage.fd = -1;
        status = uct_obmm_region_open(&devices[i], &rx->region_storage);
        if (status != UCS_OK) {
            ucs_error("obmm: failed to map export memid=%" PRIu64 ": %s",
                      devices[i].memid, ucs_status_string(status));
            goto out_release;
        }
        rx->region_opened = 1;

        status = uct_obmm_iface_try_attach_rx(iface, &rx->region_storage,
                                              fifo_stride);
        if (status == UCS_OK) {
            goto out_release;
        }

        uct_obmm_region_close(&rx->region_storage);
        rx->region_opened = 0;

        if (status != UCS_ERR_NO_RESOURCE) {
            goto out_release;
        }
    }

    ucs_error("obmm: no free export block available for this process "
              "(exports=%u)", num_exports);
    status = UCS_ERR_NO_RESOURCE;

out_release:
    uct_obmm_sysfs_release(devices);
    return status;
}


static void uct_obmm_iface_release_rx(uct_obmm_iface_rx_t *rx)
{
    if (rx->block.hdr != NULL) {
        uct_obmm_block_release(&rx->block);
    }

    if (rx->region_opened) {
        uct_obmm_region_close(&rx->region_storage);
        rx->region_opened = 0;
    }

    memset(&rx->block, 0, sizeof(rx->block));
    rx->region     = NULL;
    rx->recv_ctl   = NULL;
    rx->recv_elems = NULL;
    rx->active     = 0;
}


static UCS_CLASS_INIT_FUNC(uct_obmm_iface_t, uct_md_h tl_md, uct_worker_h worker,
                           const uct_iface_params_t *params,
                           const uct_iface_config_t *tl_config)
{
    uct_obmm_iface_config_t *config = ucs_derived_of(tl_config,
                                                     uct_obmm_iface_config_t);
    uct_obmm_md_t           *md     = ucs_derived_of(tl_md, uct_obmm_md_t);
    size_t                   fifo_stride;
    ucs_status_t             status;

    memset(&self->rx, 0, sizeof(self->rx));
    self->rx.region_storage.fd = -1;
    self->base_initialized    = 0;
    self->arbiter_initialized = 0;

    UCT_CHECK_PARAM(params->field_mask & UCT_IFACE_PARAM_FIELD_OPEN_MODE,
                    "UCT_IFACE_PARAM_FIELD_OPEN_MODE is not defined");
    if (!(params->open_mode & UCT_IFACE_OPEN_MODE_DEVICE)) {
        ucs_error("only UCT_IFACE_OPEN_MODE_DEVICE is supported");
        return UCS_ERR_UNSUPPORTED;
    }
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

    fifo_stride = uct_obmm_fifo_stride(config->fifo_size,
                                       config->fifo_elem_size);

    UCS_CLASS_CALL_SUPER_INIT(uct_base_iface_t, &uct_obmm_iface_ops,
                              &uct_obmm_iface_internal_ops, tl_md, worker,
                              params, &config->super.super
                              UCS_STATS_ARG((params->field_mask &
                                             UCT_IFACE_PARAM_FIELD_STATS_ROOT) ?
                                            params->stats_root : NULL)
                              UCS_STATS_ARG(params->mode.device.dev_name));
    self->base_initialized          = 1;

    self->config.bandwidth         = config->super.bandwidth;
    self->config.short_overhead    = config->short_overhead;
    self->config.bcopy_overhead    = config->bcopy_overhead;
    self->fifo_size                = config->fifo_size;
    self->fifo_mask                = config->fifo_size - 1u;
    self->fifo_elem_size           = config->fifo_elem_size;
    self->bcopy_seg_size           = config->bcopy_seg_size;
    self->fifo_min_poll            = config->fifo_min_poll;
    self->fifo_max_poll            = config->fifo_max_poll;
    self->pending_quota            = config->pending_quota;
    ucs_arbiter_init(&self->arbiter);
    self->arbiter_initialized = 1;

    status = uct_obmm_iface_attach_rx(self, md, fifo_stride);
    if (status != UCS_OK) {
        return status;
    }
    return UCS_OK;
}


static UCS_CLASS_CLEANUP_FUNC(uct_obmm_iface_t)
{
    if (self->base_initialized) {
        uct_base_iface_progress_disable(&self->super.super,
                                        UCT_PROGRESS_SEND |
                                        UCT_PROGRESS_RECV);
    }
    uct_obmm_iface_release_rx(&self->rx);
    /* All eps were destroyed before iface cleanup (UCX framework
     * contract; mm relies on the same), so the arbiter is empty. */
    if (self->arbiter_initialized) {
        ucs_arbiter_cleanup(&self->arbiter);
        self->arbiter_initialized = 0;
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
    .ep_am_short_iov          = uct_base_ep_am_short_iov,
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
    .iface_progress_enable    = uct_base_iface_progress_enable,
    .iface_progress_disable   = uct_base_iface_progress_disable,
    .iface_progress           = uct_obmm_iface_progress,
    .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_obmm_iface_t),
    .iface_query              = uct_obmm_iface_query,
    .iface_get_device_address = uct_obmm_iface_get_device_address,
    .iface_get_address        = ucs_empty_function_return_success,
    .iface_is_reachable       = uct_base_iface_is_reachable
};


static uct_iface_internal_ops_t uct_obmm_iface_internal_ops = {
    .iface_estimate_perf   = uct_obmm_iface_estimate_perf,
    .iface_vfs_refresh     = uct_obmm_iface_vfs_refresh,
    .ep_query              = uct_obmm_ep_query,
    .ep_invalidate         = (uct_ep_invalidate_func_t)ucs_empty_function_return_unsupported,
    .ep_connect_to_ep_v2   = ucs_empty_function_return_unsupported,
    .iface_is_reachable_v2 = uct_obmm_iface_is_reachable_v2,
    .ep_is_connected       = uct_obmm_ep_is_connected
};


UCT_TL_DEFINE_ENTRY(&uct_obmm_component, obmm,
                    uct_obmm_iface_query_tl_devices,
                    uct_obmm_iface_t, "OBMM_",
                    uct_obmm_iface_config_table,
                    uct_obmm_iface_config_t);

UCT_SINGLE_TL_INIT(&uct_obmm_component, obmm,,,)
