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
#include <ucs/sys/math.h>
#include <ucs/sys/sys.h>
#include <ucs/type/class.h>

/* obmm_set_ownership via dlopen -- see obmm_region.h */

#include <unistd.h>
#include <stdint.h>


static uct_iface_ops_t          uct_obmm_iface_ops;
static uct_iface_internal_ops_t uct_obmm_iface_internal_ops;

#define UCT_OBMM_DEVICE_NAME "memory"


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

    {"FIFO_SIZE", "128",
     "Number of elements in the per-iface receive FIFO ring (power of 2). "
     "The shared FIFO carries both am_short and am_bcopy publications.",
     ucs_offsetof(uct_obmm_iface_config_t, fifo_size), UCS_CONFIG_TYPE_UINT},

    {"FIFO_ELEM_SIZE", "2048",
     "Size in bytes of a single FIFO element. This controls am_short capacity. "
     "Keep this stride 64-byte aligned.",
        ucs_offsetof(uct_obmm_iface_config_t, fifo_elem_size),
        UCS_CONFIG_TYPE_UINT},

    {"BCOPY_SEG_SIZE", "19776",
     "Size in bytes of each per-FIFO-elem bcopy descriptor. This is "
     "advertised as max_bcopy.",
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

    {"CC_ENABLE", "0",
     "Enable CC (cache-coherent) acceleration for large am_bcopy messages. "
     "Requires OBMM_CC_MEMIDS to be set in the MD config.",
     ucs_offsetof(uct_obmm_iface_config_t, cc_enable),
     UCS_CONFIG_TYPE_INT},

    {"CC_THRESH", "32768",
     "Message size threshold in bytes above which am_bcopy tries the CC "
     "path instead of the NC desc area.  Below this, NC bcopy is used as "
     "before.",
     ucs_offsetof(uct_obmm_iface_config_t, cc_thresh),
     UCS_CONFIG_TYPE_UINT},

    {"CC_BUF_SIZE", "2097152",
     "CC buffer size in bytes.  Must be PMD_SIZE-aligned (2 MiB default). "
     "One CC buffer corresponds to one NC FIFO slot; the total CC footprint "
     "is FIFO_SIZE * CC_BUF_SIZE bytes.",
     ucs_offsetof(uct_obmm_iface_config_t, cc_buf_size),
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

    attr->cap.am.max_short       =
        uct_obmm_fifo_max_short(iface->fifo_elem_size);
    attr->cap.am.max_bcopy       = iface->bcopy_seg_size;
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
    attr->overhead               = 100e-9;
    attr->priority               = 0;
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
    iaddr->fifo_size        = iface->fifo_size;
    iaddr->fifo_elem_size   = iface->fifo_elem_size;
    iaddr->bcopy_seg_size   = iface->bcopy_seg_size;
    iaddr->cc_enabled       = iface->cc_enabled ? 1u : 0u;
    iaddr->cc_buf_size      = iface->cc_buf_size;
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

    if ((iaddr->slot_count != UCT_OBMM_POOL_SLOT_COUNT) ||
        (iaddr->short_lane_count != UCT_OBMM_SHORT_LANE_COUNT) ||
        (iaddr->fifo_size != iface->fifo_size) ||
        (iaddr->fifo_elem_size != iface->fifo_elem_size) ||
        (iaddr->bcopy_seg_size != iface->bcopy_seg_size)) {
        uct_iface_fill_info_str_buf(params,
                                    "incompatible OBMM NC geometry "
                                    "(peer slots=%u lanes=%u fifo=%u "
                                    "elem=%u seg=%u, local slots=%u "
                                    "lanes=%u fifo=%u elem=%u seg=%u)",
                                    iaddr->slot_count,
                                    iaddr->short_lane_count,
                                    iaddr->fifo_size, iaddr->fifo_elem_size,
                                    iaddr->bcopy_seg_size,
                                    UCT_OBMM_POOL_SLOT_COUNT,
                                    UCT_OBMM_SHORT_LANE_COUNT,
                                    iface->fifo_size, iface->fifo_elem_size,
                                    iface->bcopy_seg_size);
        return 0;
    }

    /* CC geometry must match if either side has CC enabled */
    if (((iaddr->cc_enabled != 0) != (iface->cc_enabled != 0)) ||
        (iaddr->cc_enabled && (iaddr->cc_buf_size != iface->cc_buf_size))) {
        uct_iface_fill_info_str_buf(params,
                                    "incompatible OBMM CC geometry "
                                    "(peer cc_en=%u cc_buf=%u, "
                                    "local cc_en=%d cc_buf=%u)",
                                    iaddr->cc_enabled, iaddr->cc_buf_size,
                                    iface->cc_enabled, iface->cc_buf_size);
        return 0;
    }

    eid.hi = daddr->exporter_deid_hi;
    eid.lo = daddr->exporter_deid_lo;

    export_r = uct_obmm_md_export_region(md);
    if ((export_r != NULL) &&
        (export_r->info.exporter_dcna == daddr->exporter_dcna) &&
        (export_r->info.exporter_deid.hi == eid.hi) &&
        (export_r->info.exporter_deid.lo == eid.lo)) {
        return uct_iface_scope_is_reachable(tl_iface, params);
    }

    if (uct_obmm_md_find_import_region(md, daddr->exporter_dcna, &eid) != NULL) {
        return uct_iface_scope_is_reachable(tl_iface, params);
    }

    uct_iface_fill_info_str_buf(params,
                                "no mapped NC region for peer dcna=0x%lx "
                                "deid=0x%lx:0x%lx",
                                (unsigned long)daddr->exporter_dcna,
                                (unsigned long)eid.hi, (unsigned long)eid.lo);
    return 0;
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

    while (polled < max_poll) {
        elem = uct_obmm_slot_elem(iface->recv_elems, iface->read_index,
                                  iface->fifo_mask, iface->fifo_elem_size);

        expected_owner = (iface->read_index & iface->fifo_size) ?
                         0u : UCT_OBMM_FIFO_ELEM_FLAG_OWNER;

        flags = elem->flags;
        if ((flags & UCT_OBMM_FIFO_ELEM_FLAG_OWNER) != expected_owner) {
            break;
        }

        ucs_memory_bus_load_fence();

        if (elem->generation != iface->generation) {
            ucs_trace_data("obmm: drop stale elem (gen=%u expected=%u) "
                           "at idx=%lu", elem->generation, iface->generation,
                           (unsigned long)iface->read_index);
        } else if (flags & UCT_OBMM_FIFO_ELEM_FLAG_CC) {
            /* CC payload: sender's CC export accessed through our cached
             * CC import (set by the first CC ep_create). */
            if (ucs_unlikely((!iface->cc_enabled) ||
                             (iface->cc_peer_region == NULL))) {
                ucs_error("obmm: received CC element but CC is "
                          "not enabled or no peer region cached");
            } else if (ucs_unlikely(elem->length > iface->cc_buf_size)) {
                ucs_error("obmm: invalid CC length %u (max=%u)",
                          elem->length, iface->cc_buf_size);
            } else {
                uint64_t cc_offset = elem->header;
                void    *cc_ptr    = (char*)iface->cc_peer_region->base
                                     + cc_offset;
                int      ret;

                ret = uct_obmm_cc_set_ownership(iface->cc_peer_region->fd,
                                         cc_ptr,
                                         (char*)cc_ptr + elem->length,
                                         PROT_READ);
                if (ret != 0) {
                    ucs_error("obmm: CC recv set_ownership(READ) "
                              "at offset 0x%" PRIx64 " failed: %m",
                              cc_offset);
                } else {
                    uct_iface_invoke_am(&iface->super, elem->am_id,
                                        cc_ptr, elem->length, 0);
                    ret = uct_obmm_cc_set_ownership(iface->cc_peer_region->fd,
                                             cc_ptr,
                                             (char*)cc_ptr + elem->length,
                                             PROT_NONE);
                    if (ret != 0) {
                        ucs_error("obmm: CC recv set_ownership(NONE) "
                                  "at offset 0x%" PRIx64 " failed: %m",
                                  cc_offset);
                    }
                }
            }
        } else if (flags & UCT_OBMM_FIFO_ELEM_FLAG_BCOPY) {
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
        uct_obmm_bus_full_fence();
        iface->recv_ctl->tail = iface->read_index;
    }

    ucs_arbiter_dispatch(&iface->arbiter, 1, uct_obmm_ep_process_pending,
                         &pending_progress);

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
    uct_obmm_region_t       *region;
    uct_obmm_region_t       *cc_region;
    size_t                   stride;
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
    if (uct_obmm_fifo_max_short(config->fifo_elem_size) > UINT16_MAX) {
        ucs_error("obmm: FIFO_ELEM_SIZE (%u) too large; max_short must fit "
                  "in uint16 (max %u)",
                  config->fifo_elem_size, (unsigned)UINT16_MAX);
        return UCS_ERR_INVALID_PARAM;
    }
    if (config->bcopy_seg_size == 0) {
        ucs_error("obmm: BCOPY_SEG_SIZE must be > 0");
        return UCS_ERR_INVALID_PARAM;
    }
    if (config->bcopy_seg_size > UINT16_MAX) {
        ucs_error("obmm: BCOPY_SEG_SIZE (%u) too large; max_bcopy must fit "
                  "in uint16 (max %u)",
                  config->bcopy_seg_size, (unsigned)UINT16_MAX);
        return UCS_ERR_INVALID_PARAM;
    }

    region = uct_obmm_md_export_region(md);
    if (region == NULL) {
        ucs_error("obmm: cannot create iface; this MD has no local NC export "
                  "region");
        return UCS_ERR_NO_DEVICE;
    }

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
        ucs_error("obmm: geometry does not fit in NC region: "
                  "fifo_size=%u elem_size=%u seg_size=%u stride=%zu "
                  "slot_count=%u required=%zu region=%zu. "
                  "Reduce UCX_OBMM_BCOPY_SEG_SIZE, UCX_OBMM_FIFO_SIZE, or "
                  "UCX_OBMM_FIFO_ELEM_SIZE.",
                  config->fifo_size, config->fifo_elem_size,
                  config->bcopy_seg_size, stride,
                  UCT_OBMM_POOL_SLOT_COUNT, required, region->length);
        return UCS_ERR_INVALID_PARAM;
    }

    /* CC validation */
    cc_region = NULL;
    if (config->cc_enable) {
        cc_region = uct_obmm_md_cc_export_region(md);
        if (cc_region == NULL) {
            ucs_error("obmm: CC_ENABLE=1 but no CC export region found; "
                      "set OBMM_CC_MEMIDS");
            return UCS_ERR_NO_DEVICE;
        }
        if (config->cc_buf_size == 0) {
            ucs_error("obmm: CC_BUF_SIZE must be > 0");
            return UCS_ERR_INVALID_PARAM;
        }
        if ((config->cc_buf_size & (config->cc_buf_size - 1)) != 0) {
            ucs_warn("obmm: CC_BUF_SIZE (%u) is not a power of 2; "
                     "rounding up", config->cc_buf_size);
        }
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

    self->region           = region;
    self->config.bandwidth = config->super.bandwidth;
    self->fifo_size        = config->fifo_size;
    self->fifo_mask        = config->fifo_size - 1u;
    self->fifo_elem_size   = config->fifo_elem_size;
    self->bcopy_seg_size   = config->bcopy_seg_size;
    self->fifo_min_poll    = config->fifo_min_poll;
    self->fifo_max_poll    = config->fifo_max_poll;
    self->fifo_poll_count  = config->fifo_min_poll;
    self->fifo_prev_wnd_cons = 0;
    self->pending_quota    = config->pending_quota;
    self->read_index       = 0;

    self->cc_enabled       = config->cc_enable && (cc_region != NULL);
    self->cc_thresh        = config->cc_thresh;
    self->cc_buf_size      = config->cc_buf_size;
    self->cc_region        = cc_region;
    self->cc_peer_region   = NULL;

    if (self->cc_enabled) {
        self->cc_num_bufs  = (unsigned)(cc_region->length / self->cc_buf_size);
        if (self->cc_num_bufs > self->fifo_size) {
            self->cc_num_bufs = self->fifo_size;
        }
        if (self->cc_num_bufs == 0) {
            ucs_error("obmm: CC region too small (%zu bytes) for "
                      "cc_buf_size=%u", cc_region->length, self->cc_buf_size);
            return UCS_ERR_INVALID_PARAM;
        }
        ucs_debug("obmm: CC enabled: region=%p (memid=%" PRIu64 ") "
                  "buf_size=%u num_bufs=%u thresh=%u",
                  cc_region->base, cc_region->info.memid,
                  self->cc_buf_size, self->cc_num_bufs, self->cc_thresh);
    }

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

    ucs_arbiter_init(&self->arbiter);

    ucs_debug("obmm: iface %p attached to NC region %p slot=%u gen=%u "
              "fifo_size=%u elem_size=%u seg_size=%u stride=%zu "
              "cc_enabled=%d",
              self, region->base, self->slot_index, self->generation,
              self->fifo_size, self->fifo_elem_size, self->bcopy_seg_size,
              stride, self->cc_enabled);
    return UCS_OK;
}


static UCS_CLASS_CLEANUP_FUNC(uct_obmm_iface_t)
{
    uct_base_iface_progress_disable(&self->super.super,
                                    UCT_PROGRESS_SEND | UCT_PROGRESS_RECV);
    if ((self->pool.hdr != NULL) &&
        uct_obmm_pool_free_slot(&self->pool, self->slot_index)) {
        uct_obmm_pool_reset(&self->pool);
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


UCT_TL_DEFINE_ENTRY(&uct_obmm_component, obmm, uct_obmm_iface_query_tl_devices,
                    uct_obmm_iface_t, "OBMM_", uct_obmm_iface_config_table,
                    uct_obmm_iface_config_t);

UCT_SINGLE_TL_INIT(&uct_obmm_component, obmm,,,)