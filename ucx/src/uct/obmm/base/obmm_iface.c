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
#include <ucs/sys/math.h>
#include <ucs/sys/sys.h>
#include <ucs/type/class.h>

#include <stdint.h>
#include <string.h>
#include <unistd.h>


static uct_iface_ops_t          uct_obmm_iface_ops;
static uct_iface_internal_ops_t uct_obmm_iface_internal_ops;

#define UCT_OBMM_DEVICE_NAME "memory"

static UCS_F_ALWAYS_INLINE unsigned
uct_obmm_iface_progress_lane(uct_obmm_iface_t *iface, uct_obmm_rx_lane_t *lane);


static unsigned
uct_obmm_iface_progress_unregistered_lanes(uct_obmm_iface_t *iface,
                                           unsigned max_poll)
{
    uct_obmm_md_t         *md = ucs_derived_of(iface->super.md, uct_obmm_md_t);
    uct_obmm_pool_t        pool;
    uct_obmm_region_t     *region;
    uct_obmm_rx_lane_t     lane;
    uct_obmm_mailbox_ctl_t *ctl;
    ucs_status_t           status;
    void                  *slot_base;
    void                  *lane_base;
    unsigned               i;
    unsigned               slot;
    unsigned               bank;
    unsigned               completions = 0;
    uint32_t               head;
    uint32_t               tail;

    for (i = 0; i < md->num_regions; ++i) {
        region = &md->regions[i];
        bank   = (i == (unsigned)md->export_idx) ? UCT_OBMM_MAILBOX_BANK_LOCAL :
                                                   UCT_OBMM_MAILBOX_BANK_REMOTE;

        status = uct_obmm_pool_open(region->base, region->length, &pool);
        if (status != UCS_OK) {
            continue;
        }

        for (slot = 0; (slot < pool.slot_count) &&
                       (slot < UCT_OBMM_POOL_SLOT_COUNT); ++slot) {
            if ((bank == UCT_OBMM_MAILBOX_BANK_LOCAL) &&
                (slot == iface->slot_index)) {
                continue;
            }
            if (iface->rx_lanes[bank][slot].active) {
                continue;
            }
            if (pool.meta[slot].state != UCT_OBMM_SLOT_STATE_IN_USE) {
                continue;
            }

            slot_base = uct_obmm_pool_slot_ptr(&pool, slot);
            lane_base = uct_obmm_slot_lane(slot_base, bank, iface->slot_index,
                                           iface->fifo_size,
                                           iface->fifo_elem_size,
                                           iface->bcopy_seg_size);
            ctl = uct_obmm_lane_ctl(lane_base);

            ucs_memory_bus_load_fence();
            if (ctl->sender_generation != pool.meta[slot].generation) {
                continue;
            }

            head = ctl->head;
            tail = ctl->tail;
            if (head == tail) {
                continue;
            }

            memset(&lane, 0, sizeof(lane));
            lane.ctl               = ctl;
            lane.elems             = uct_obmm_lane_elems(lane_base);
            lane.descs             = uct_obmm_lane_descs(lane_base,
                                                         iface->fifo_size,
                                                         iface->fifo_elem_size);
            lane.sender_generation = pool.meta[slot].generation;
            lane.rx_index          = (ctl->tail_generation == lane.sender_generation) ?
                                     tail : 0;
            lane.active            = 1;

            completions += uct_obmm_iface_progress_lane(iface, &lane);
            if (completions >= max_poll) {
                return completions;
            }
        }
    }

    return completions;
}


ucs_config_field_t uct_obmm_iface_config_table[] = {
    {"", "", NULL, ucs_offsetof(uct_obmm_iface_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_iface_config_table)},

    {"BW", "3400MBs",
     "Effective transport bandwidth used for UCP lane/protocol cost "
     "modeling. This is not a required knob: if the user does not set "
     "UCX_OBMM_BW, obmm uses this sustained default.",
     ucs_offsetof(uct_obmm_iface_config_t, super.bandwidth), UCS_CONFIG_TYPE_BW},

    {"FIFO_SIZE", "1",
     "Mailbox depth per sender->receiver lane (power of 2). "
     "Depth 1 keeps the new sender-owned SPSC mailbox layout within the "
     "current 128 MiB region budget.",
     ucs_offsetof(uct_obmm_iface_config_t, fifo_size), UCS_CONFIG_TYPE_UINT},

    {"FIFO_ELEM_SIZE", "16448",
     "Size in bytes of a single mailbox element. Must be greater than "
     "sizeof(uct_obmm_fifo_element_t) (=16). Caps the total am_short "
     "(header + payload) bytes at (FIFO_ELEM_SIZE - 16). Defaults keep "
     "16KiB-class payloads comfortably on the short path while preserving "
     "64-byte alignment for every element stride.",
        ucs_offsetof(uct_obmm_iface_config_t, fifo_elem_size),
        UCS_CONFIG_TYPE_UINT},

    {"BCOPY_SEG_SIZE", "32768",
     "Size in bytes of each per-element bcopy descriptor. This is "
     "advertised as max_bcopy. Defaults keep raw UCT bcopy at 32KiB for "
     "common medium-message eager traffic, while preserving 64-byte "
     "alignment for every descriptor stride. Capped at 65535 "
     "(elem->length is uint16).",
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
    return uct_single_device_resource(md, UCT_OBMM_DEVICE_NAME,
                                      UCT_DEVICE_TYPE_SHM,
                                      UCS_SYS_DEVICE_ID_UNKNOWN, tl_devices_p,
                                      num_tl_devices_p);
}


static ucs_status_t uct_obmm_iface_query(uct_iface_h tl_iface,
                                         uct_iface_attr_t *attr)
{
    uct_obmm_iface_t *iface     = ucs_derived_of(tl_iface, uct_obmm_iface_t);
    size_t            elem_hdr  = sizeof(uct_obmm_fifo_element_t);

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


static UCS_F_ALWAYS_INLINE unsigned
uct_obmm_iface_progress_lane(uct_obmm_iface_t *iface, uct_obmm_rx_lane_t *lane)
{
    uct_obmm_fifo_element_t *elem;
    uint32_t                 head;

    if (!lane->active) {
        return 0;
    }

    ucs_memory_bus_load_fence();
    if (lane->ctl->sender_generation != lane->sender_generation) {
        return 0;
    }

    head = lane->ctl->head;
    if (head == lane->rx_index) {
        return 0;
    }

    elem = uct_obmm_lane_elem(lane->elems, lane->rx_index, iface->fifo_mask,
                              iface->fifo_elem_size);
    ucs_memory_bus_load_fence();

    if (elem->generation == iface->generation) {
        if (elem->flags & UCT_OBMM_MAILBOX_ELEM_FLAG_BCOPY) {
            void *desc = uct_obmm_lane_desc(lane->descs, lane->rx_index,
                                            iface->fifo_mask,
                                            iface->bcopy_seg_size);
            uct_iface_invoke_am(&iface->super, elem->am_id,
                                desc, elem->length, 0);
        } else {
            uct_iface_invoke_am(&iface->super, elem->am_id,
                                &elem->header, elem->length, 0);
        }
    }

    lane->rx_index++;
    uct_obmm_bus_full_fence();
    if (lane->ctl->sender_generation != lane->sender_generation) {
        return 1;
    }
    lane->ctl->tail = lane->rx_index;
    ucs_memory_bus_store_fence();
    if (lane->ctl->sender_generation != lane->sender_generation) {
        return 1;
    }
    lane->ctl->tail_generation = lane->sender_generation;
    return 1;
}


static unsigned uct_obmm_iface_progress(uct_iface_h tl_iface)
{
    uct_obmm_iface_t *iface = ucs_derived_of(tl_iface, uct_obmm_iface_t);
    unsigned          polled = 0;
    unsigned          total_lanes = UCT_OBMM_MAILBOX_BANK_COUNT *
                                    UCT_OBMM_POOL_SLOT_COUNT;
    unsigned          scanned;

    for (scanned = 0; (scanned < total_lanes) && (polled < iface->fifo_max_poll);
         ++scanned) {
        unsigned           idx  = (iface->rx_lane_rr + scanned) % total_lanes;
        unsigned           bank = idx / UCT_OBMM_POOL_SLOT_COUNT;
        unsigned           slot = idx % UCT_OBMM_POOL_SLOT_COUNT;

        polled += uct_obmm_iface_progress_lane(iface,
                                               &iface->rx_lanes[bank][slot]);
    }

    iface->rx_lane_rr = (iface->rx_lane_rr + 1) % total_lanes;

    if (polled < iface->fifo_max_poll) {
        polled += uct_obmm_iface_progress_unregistered_lanes(iface,
                                                             iface->fifo_max_poll - polled);
    }

    ucs_arbiter_dispatch(&iface->arbiter, 1, uct_obmm_ep_process_pending,
                         &polled);

    return polled;
}


static ucs_status_t uct_obmm_iface_fence(uct_iface_h tl_iface, unsigned flags)
{
    (void)flags;
    uct_obmm_bus_full_fence();
    UCT_TL_IFACE_STAT_FENCE(ucs_derived_of(tl_iface, uct_base_iface_t));
    return UCS_OK;
}


static ucs_status_t uct_obmm_ep_fence(uct_ep_h tl_ep, unsigned flags)
{
    (void)flags;
    uct_obmm_bus_full_fence();
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
    size_t                   stride;
    size_t                   required;
    ucs_status_t             status;
    unsigned                 bank;
    unsigned                 slot;

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
        ucs_error("obmm: mailbox geometry does not fit in region: "
                  "fifo_size=%u elem_size=%u seg_size=%u stride=%zu "
                  "slot_count=%u required=%zu region=%zu",
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

    self->region           = region;
    self->config.bandwidth = config->super.bandwidth;
    self->fifo_size        = config->fifo_size;
    self->fifo_mask        = config->fifo_size - 1u;
    self->fifo_elem_size   = config->fifo_elem_size;
    self->bcopy_seg_size   = config->bcopy_seg_size;
    self->fifo_max_poll    = (config->fifo_max_poll == 0) ? 1 :
                             config->fifo_max_poll;
    self->rx_lane_rr       = 0;
    memset(self->rx_lanes, 0, sizeof(self->rx_lanes));

    status = uct_obmm_pool_attach(region->base, region->length,
                                  UCT_OBMM_POOL_SLOT_COUNT,
                                  (uint32_t)stride, &self->pool);
    if (status != UCS_OK) {
        ucs_error("obmm: pool attach failed: %s", ucs_status_string(status));
        return status;
    }

    status = uct_obmm_pool_alloc_slot(&self->pool, &self->slot_index,
                                      &self->slot, &self->generation);
    if (status != UCS_OK) {
        ucs_error("obmm: failed to allocate mailbox slot: %s",
                  ucs_status_string(status));
        return status;
    }

    for (bank = 0; bank < UCT_OBMM_MAILBOX_BANK_COUNT; ++bank) {
        for (slot = 0; slot < UCT_OBMM_POOL_SLOT_COUNT; ++slot) {
            void *lane = uct_obmm_slot_lane(self->slot, bank, slot,
                                            self->fifo_size,
                                            self->fifo_elem_size,
                                            self->bcopy_seg_size);
            uct_obmm_lane_ctl(lane)->sender_generation = self->generation;
        }
    }
    ucs_memory_bus_store_fence();

    ucs_arbiter_init(&self->arbiter);

    ucs_debug("obmm: iface %p attached to region %p slot=%u gen=%u "
              "fifo_size=%u elem_size=%u seg_size=%u stride=%zu",
              self, region->base, self->slot_index, self->generation,
              self->fifo_size, self->fifo_elem_size, self->bcopy_seg_size,
              stride);
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
