/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_IFACE_H_
#define UCT_OBMM_IFACE_H_

#include "obmm_md.h"
#include "obmm_pool.h"
#include "obmm_fifo.h"

#include <uct/base/uct_iface.h>
#include <ucs/datastruct/arbiter.h>


/* Wire-format device address: identifies the obmm-side fabric coordinates of
 * the iface's owning region. */
typedef struct uct_obmm_device_addr {
    uint64_t exporter_dcna;
    uint64_t exporter_deid_hi;
    uint64_t exporter_deid_lo;
} uct_obmm_device_addr_t;


/* Wire-format iface address: identifies the receiver-owned slot inside the
 * region named by the device address, plus enough geometry for the peer to
 * validate compatibility before trusting any pointer math. */
typedef struct uct_obmm_iface_addr {
    uint32_t slot_index;
    uint32_t generation;
    uint32_t pid;
    uint32_t layout;          /* UCT_OBMM_FIFO_LAYOUT_xx */
    uint32_t shard_count;     /* shards per bank inside the receiver slot */
    uint32_t fifo_size;       /* depth per shard (power of 2) */
    uint32_t fifo_elem_size;
    uint32_t bcopy_seg_size;
} uct_obmm_iface_addr_t;


typedef struct uct_obmm_iface_common_config {
    uct_iface_config_t     super;
    double                 bandwidth; /* Effective transport bandwidth in
                                         bytes/s for UCP cost modeling */
} uct_obmm_iface_common_config_t;


typedef struct uct_obmm_iface_config {
    uct_obmm_iface_common_config_t super;
    unsigned                       shard_count;     /* shards per bank (power of 2) */
    unsigned                       fifo_size;       /* depth per shard (power of 2) */
    unsigned                       fifo_elem_size;  /* bytes per FIFO element (incl. hdr) */
    unsigned                       bcopy_seg_size;  /* bytes per paired bulk buffer */
    size_t                         fifo_max_poll;   /* RX completions per progress() */
} uct_obmm_iface_config_t;


typedef struct uct_obmm_rx_shard {
    uct_obmm_fifo_ctl_t *ctl;
    void                *elems;
    void                *descs;
    uint64_t             rx_index;
} uct_obmm_rx_shard_t;


typedef struct uct_obmm_iface {
    uct_base_iface_t         super;
    struct {
        double               bandwidth; /* Effective transport bandwidth in
                                           bytes/s for UCP cost modeling */
    } config;

    /* Local receiver-owned slot inside the local export region. */
    uct_obmm_pool_t          pool;            /* attached local export pool */
    uct_obmm_region_t       *region;          /* points into md->regions[]  */
    void                    *slot;            /* base of our receiver slot   */
    uint32_t                 slot_index;      /* our slot index in pool        */
    uint32_t                 generation;      /* our slot generation token     */

    /* Geometry, cached from config. fifo_size and shard_count MUST be powers
     * of 2. */
    unsigned                 shard_count;
    unsigned                 shard_mask;      /* shard_count - 1              */
    unsigned                 fifo_size;
    unsigned                 fifo_mask;       /* fifo_size - 1                */
    unsigned                 fifo_elem_size;
    unsigned                 bcopy_seg_size;  /* == max_bcopy, backed by paired bulk buffer */
    size_t                   fifo_max_poll;

    /* Local receiver shard state keyed by (bank, shard). */
    uct_obmm_rx_shard_t      rx_shards[UCT_OBMM_FIFO_BANK_COUNT]
                                      [UCT_OBMM_MAX_FIFO_SHARDS];
    unsigned                 rx_shard_rr;

    /* Pending send arbiter (mirrors mm). pending_add queues UCP requests here
     * when the remote receiver shard is full; iface_progress dispatches them
     * every call. */
    ucs_arbiter_t            arbiter;
} uct_obmm_iface_t;


extern ucs_config_field_t uct_obmm_iface_config_table[];

ucs_status_t
uct_obmm_iface_query_tl_devices(uct_md_h md,
                                uct_tl_device_resource_t **tl_devices_p,
                                unsigned *num_tl_devices_p);

UCS_CLASS_DECLARE_NEW_FUNC(uct_obmm_iface_t, uct_iface_t, uct_md_h, uct_worker_h,
                           const uct_iface_params_t*, const uct_iface_config_t*);

#endif
