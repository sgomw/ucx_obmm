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

#include <unistd.h>
#include <stdint.h>


static uct_iface_ops_t          uct_obmm_iface_ops;
static uct_iface_internal_ops_t uct_obmm_iface_internal_ops;


ucs_config_field_t uct_obmm_iface_config_table[] = {
    {"", "", NULL, ucs_offsetof(uct_obmm_iface_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_sm_iface_config_table)},

    {"FIFO_SIZE", "64",
     "Number of elements in the per-iface receive FIFO ring (power of 2).",
     ucs_offsetof(uct_obmm_iface_config_t, fifo_size), UCS_CONFIG_TYPE_UINT},

    {"FIFO_ELEM_SIZE", "2048",
     "Size in bytes of a single FIFO element. Must be greater than "
     "sizeof(uct_obmm_fifo_element_t) (=16). Caps am_short payload at "
     "(FIFO_ELEM_SIZE - 16).",
     ucs_offsetof(uct_obmm_iface_config_t, fifo_elem_size),
     UCS_CONFIG_TYPE_UINT},

    {"BCOPY_SEG_SIZE", "4096",
     "Size in bytes of each per-FIFO-elem bcopy descriptor. This is "
     "advertised as max_bcopy. Larger values reduce UCP fragmentation "
     "for medium messages but consume more of the 128 MiB region "
     "(per-slot footprint = FIFO_SIZE * (FIFO_ELEM_SIZE + "
     "BCOPY_SEG_SIZE)). Capped at 65535 (elem->length is uint16).",
     ucs_offsetof(uct_obmm_iface_config_t, bcopy_seg_size),
     UCS_CONFIG_TYPE_UINT},

    {"FIFO_MAX_POLL", "16",
     "Maximum receive completions to drain in one progress() call.",
     ucs_offsetof(uct_obmm_iface_config_t, fifo_max_poll),
     UCS_CONFIG_TYPE_ULUNITS},

    {NULL}
};


ucs_status_t
uct_obmm_iface_query_tl_devices(uct_md_h md,
                                uct_tl_device_resource_t **tl_devices_p,
                                unsigned *num_tl_devices_p)
{
    return uct_sm_base_query_tl_devices(md, tl_devices_p, num_tl_devices_p);
}


static ucs_status_t uct_obmm_iface_query(uct_iface_h tl_iface,
                                         uct_iface_attr_t *attr)
{
    uct_obmm_iface_t *iface     = ucs_derived_of(tl_iface, uct_obmm_iface_t);
    size_t            elem_hdr  = sizeof(uct_obmm_fifo_element_t);

    uct_base_iface_query(&iface->super.super, attr);
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
     * (header + payload), i.e. NOT counting the elem header. */
    attr->cap.am.max_short       = iface->fifo_elem_size - elem_hdr;
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
    attr->bandwidth.dedicated    = iface->super.config.bandwidth;
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

    iaddr->slot_index     = iface->slot_index;
    iaddr->generation     = iface->generation;
    iaddr->pid            = (uint32_t)getpid();
    iaddr->fifo_size      = iface->fifo_size;
    iaddr->fifo_elem_size = iface->fifo_elem_size;
    iaddr->bcopy_seg_size = iface->bcopy_seg_size;
    return UCS_OK;
}


static int
uct_obmm_iface_is_reachable_v2(const uct_iface_h tl_iface,
                               const uct_iface_is_reachable_params_t *params)
{
    uct_obmm_iface_t             *iface = ucs_derived_of(tl_iface,
                                                         uct_obmm_iface_t);
    uct_obmm_md_t                *md    = ucs_derived_of(iface->super.super.md,
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

    if ((iaddr->fifo_size != iface->fifo_size) ||
        (iaddr->fifo_elem_size != iface->fifo_elem_size) ||
        (iaddr->bcopy_seg_size != iface->bcopy_seg_size)) {
        uct_iface_fill_info_str_buf(params,
                                    "incompatible OBMM geometry "
                                    "(peer fifo=%u elem=%u seg=%u, "
                                    "local fifo=%u elem=%u seg=%u)",
                                    iaddr->fifo_size, iaddr->fifo_elem_size,
                                    iaddr->bcopy_seg_size,
                                    iface->fifo_size, iface->fifo_elem_size,
                                    iface->bcopy_seg_size);
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
                                "no mapped region for peer dcna=0x%lx "
                                "deid=0x%lx:0x%lx",
                                (unsigned long)daddr->exporter_dcna,
                                (unsigned long)eid.hi, (unsigned long)eid.lo);
    return 0;
}


static unsigned uct_obmm_iface_progress(uct_iface_h tl_iface)
{
    uct_obmm_iface_t        *iface = ucs_derived_of(tl_iface, uct_obmm_iface_t);
    unsigned                 polled = 0;
    uct_obmm_fifo_element_t *elem;
    uint8_t                  flags;
    uint8_t                  expected_owner;
    size_t                   max_poll = iface->fifo_max_poll;

    while (polled < max_poll) {
        elem = uct_obmm_slot_elem(iface->recv_elems, iface->read_index,
                                  iface->fifo_mask, iface->fifo_elem_size);

        /* Owner bit alternates each lap of the ring; pass 0 expects 1, pass
         * 1 expects 0, etc. Combined with zero-fill on slot allocation, this
         * means an unwritten slot reads as flags==0 and is correctly skipped
         * on the very first lap. */
        expected_owner = ((iface->read_index / iface->fifo_size) & 1u) ?
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
        } else if (flags & UCT_OBMM_FIFO_ELEM_FLAG_BCOPY) {
            /* am_bcopy: payload is in the paired desc[N], not in the FIFO
             * element body. The bus_load_fence above orders this load
             * with respect to the sender's bus_store_fence + flag write. */
            void *desc = uct_obmm_slot_desc(iface->recv_descs,
                                            iface->read_index,
                                            iface->fifo_mask,
                                            iface->bcopy_seg_size);
            uct_iface_invoke_am(&iface->super.super, elem->am_id,
                                desc, elem->length, 0);
        } else {
            /* am_short: contiguous [header(8B)][payload] starting at
             * &elem->header. elem->length already includes the 8B header. */
            uct_iface_invoke_am(&iface->super.super, elem->am_id,
                                &elem->header, elem->length, 0);
        }

        iface->read_index++;
        polled++;
    }

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
     * snapshot so retries see the latest state. Without this dispatch,
     * UCS_ERR_BUSY-only pending_add caused a livelock under symmetric
     * bidirectional load (osu_bibw at size==BCOPY_SEG_SIZE). */
    ucs_arbiter_dispatch(&iface->arbiter, 1, uct_obmm_ep_process_pending,
                         &polled);

    return polled;
}


static UCS_CLASS_INIT_FUNC(uct_obmm_iface_t, uct_md_h tl_md, uct_worker_h worker,
                           const uct_iface_params_t *params,
                           const uct_iface_config_t *tl_config)
{
    uct_obmm_iface_config_t *config = ucs_derived_of(tl_config,
                                                     uct_obmm_iface_config_t);
    uct_obmm_md_t           *md     = ucs_derived_of(tl_md, uct_obmm_md_t);
    uct_obmm_region_t       *region;
    size_t                   stride;
    size_t                   required;
    ucs_status_t             status;

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
    /* elem->length is uint16_t; reject geometries whose advertised
     * max_short / max_bcopy would not fit in that field. */
    if ((config->fifo_elem_size - sizeof(uct_obmm_fifo_element_t)) >
        UINT16_MAX) {
        ucs_error("obmm: FIFO_ELEM_SIZE (%u) too large; payload area must fit "
                  "in uint16 (max %zu)",
                  config->fifo_elem_size,
                  (size_t)UINT16_MAX + sizeof(uct_obmm_fifo_element_t));
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
        ucs_error("obmm: cannot create iface; this MD has no local export "
                  "region");
        return UCS_ERR_NO_DEVICE;
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
                  "Reduce UCX_OBMM_BCOPY_SEG_SIZE or UCX_OBMM_FIFO_SIZE.",
                  config->fifo_size, config->fifo_elem_size,
                  config->bcopy_seg_size, stride,
                  UCT_OBMM_POOL_SLOT_COUNT, required, region->length);
        return UCS_ERR_INVALID_PARAM;
    }

    UCS_CLASS_CALL_SUPER_INIT(uct_sm_iface_t, &uct_obmm_iface_ops,
                              &uct_obmm_iface_internal_ops, tl_md, worker,
                              params, tl_config);

    self->region         = region;
    self->fifo_size      = config->fifo_size;
    self->fifo_mask      = config->fifo_size - 1u;
    self->fifo_elem_size = config->fifo_elem_size;
    self->bcopy_seg_size = config->bcopy_seg_size;
    self->fifo_max_poll  = (config->fifo_max_poll == 0) ? 1 :
                           config->fifo_max_poll;
    self->read_index     = 0;

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

    /* recv_slot was zeroed by pool_alloc_slot, so head/tail/all element
     * flags are zero. read_index starts at 0, expected owner bit on the
     * first lap is 1; uninitialized zero correctly reads as "not yet
     * written". */

    ucs_debug("obmm: iface %p attached to region %p slot=%u gen=%u "
              "fifo_size=%u elem_size=%u seg_size=%u stride=%zu",
              self, region->base, self->slot_index, self->generation,
              self->fifo_size, self->fifo_elem_size, self->bcopy_seg_size,
              stride);
    return UCS_OK;
}


static UCS_CLASS_CLEANUP_FUNC(uct_obmm_iface_t)
{
    uct_base_iface_progress_disable(&self->super.super.super,
                                    UCT_PROGRESS_SEND | UCT_PROGRESS_RECV);
    if (self->pool.hdr != NULL) {
        uct_obmm_pool_free_slot(&self->pool, self->slot_index);
    }
    /* All eps were destroyed before iface cleanup (UCX framework
     * contract; mm relies on the same), so the arbiter is empty. */
    ucs_arbiter_cleanup(&self->arbiter);
}


UCS_CLASS_DEFINE(uct_obmm_iface_t, uct_sm_iface_t);
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
    .ep_fence                 = uct_sm_ep_fence,
    .ep_check                 = (uct_ep_check_func_t)ucs_empty_function_return_unsupported,
    .ep_create                = UCS_CLASS_NEW_FUNC_NAME(uct_obmm_ep_t),
    .ep_destroy               = UCS_CLASS_DELETE_FUNC_NAME(uct_obmm_ep_t),
    .iface_flush              = uct_base_iface_flush,
    .iface_fence              = uct_sm_iface_fence,
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
