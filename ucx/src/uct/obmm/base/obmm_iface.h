/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_IFACE_H_
#define UCT_OBMM_IFACE_H_

#include "obmm_md.h"
#include "obmm_block.h"
#include "obmm_fifo.h"

#include <stdint.h>
#include <uct/base/uct_iface.h>
#include <ucs/datastruct/arbiter.h>

#define UCT_OBMM_IFACE_FIFO_SIZE_DEFAULT     256
#define UCT_OBMM_IFACE_FIFO_MIN_POLL_DEFAULT 64
#define UCT_OBMM_IFACE_FIFO_MAX_POLL_DEFAULT 128
#define UCT_OBMM_IFACE_FIFO_AI_VALUE         1u
#define UCT_OBMM_IFACE_FIFO_MD_FACTOR        2u
#define UCT_OBMM_BCOPY_POOL_EXTRA            8u /* async bcopy spare slots */


struct uct_obmm_ep;


/* Receiver-owned pool state. The pool bytes are in the mapped NC block, while
 * the free-index stack and release descriptor are local to the receiver. */
typedef struct uct_obmm_bcopy_pool {
    void             *base;
    size_t            fifo_offset;
    size_t            slot_size;
    size_t            payload_offset;
    unsigned          num_slots;
    unsigned          free_capacity;
    unsigned          free_count;
    uint32_t          release_logged;
    unsigned         *free_indices;
    uct_recv_desc_t   release_desc;
} uct_obmm_bcopy_pool_t;


typedef struct uct_obmm_region_addr {
    uint64_t exporter_dcna;
    uint32_t exporter_deid;
    uint32_t region_id;
} UCS_S_PACKED uct_obmm_region_addr_t;


/* Wire-format device address. Keep this at 31 bytes or less so UCP's default
 * worker-address v1 format can pack it. `primary` identifies the claimed NC
 * export block. */
typedef struct uct_obmm_device_addr {
    uct_obmm_region_addr_t primary;
} UCS_S_PACKED uct_obmm_device_addr_t;


typedef struct uct_obmm_iface_common_config {
    uct_iface_config_t     super;
    double                 bandwidth; /* Effective transport bandwidth in
                                         bytes/s for UCP cost modeling */
} uct_obmm_iface_common_config_t;


typedef struct uct_obmm_iface_config {
    uct_obmm_iface_common_config_t super;
    unsigned                       fifo_size;       /* FIFO ring depth (power of 2) */
    unsigned                       fifo_elem_size;  /* bytes per element (incl. hdr) */
    unsigned                       bcopy_seg_size;  /* bytes per bcopy buffer */
    double                         short_overhead;  /* AM_SHORT per-side model */
    double                         bcopy_overhead;  /* AM_BCOPY per-side model */
    size_t                         fifo_min_poll;   /* Minimal RX completions per progress() */
    size_t                         fifo_max_poll;   /* Maximal RX completions per progress() */
    unsigned                       pending_quota;   /* Pending retries per progress() */
} uct_obmm_iface_config_t;


typedef struct uct_obmm_iface_rx {
    uct_obmm_block_t         block;
    uct_obmm_region_t        region_storage;
    uct_obmm_region_t       *region;
    uct_obmm_fifo_ctl_t     *recv_ctl;
    void                    *recv_elems;
    uint64_t                 read_index;
    uint64_t                 last_bcopy_pool_empty_index;
    uint32_t                 rx_wait_logged;
    size_t                   fifo_poll_count;
    int                      fifo_prev_wnd_cons;
    int                      region_opened;
    int                      active;
    unsigned                 rx_diag_stage;
    unsigned                 rx_bcopy_diag_stage;
    uct_obmm_bcopy_pool_t    bcopy_pool;
} uct_obmm_iface_rx_t;


typedef struct uct_obmm_iface {
    uct_base_iface_t         super;
    struct {
        double               bandwidth; /* Effective transport bandwidth in
                                           bytes/s for UCP cost modeling */
        double               short_overhead;
        double               bcopy_overhead;
    } config;

    uct_obmm_iface_rx_t      rx;

    /* Geometry, cached from config. fifo_size MUST be power of 2. */
    unsigned                 fifo_size;
    unsigned                 fifo_mask;       /* fifo_size - 1              */
    unsigned                 fifo_elem_size;
    unsigned                 bcopy_seg_size;  /* == max_bcopy              */
    size_t                   fifo_stride;
    size_t                   bcopy_pool_offset; /* relative to FIFO base     */
    size_t                   bcopy_pool_size;
    size_t                   bcopy_pool_slot_size;
    size_t                   bcopy_pool_payload_offset;
    size_t                   rx_headroom;
    size_t                   fifo_min_poll;
    size_t                   fifo_max_poll;
    unsigned                 pending_quota;

    /* Pending send arbiter (mirrors mm). pending_add queues UCP requests
     * when peer FIFO state still looks full after a normal tail refresh;
     * iface_progress dispatches them after draining receives so newly
     * published tails become visible to retries. */
    ucs_arbiter_t            arbiter;
} uct_obmm_iface_t;


extern ucs_config_field_t uct_obmm_iface_config_table[];

ucs_status_t
uct_obmm_iface_query_tl_devices(uct_md_h md,
                                uct_tl_device_resource_t **tl_devices_p,
                                unsigned *num_tl_devices_p);

ucs_status_t
uct_obmm_iface_resolve_peer(uct_obmm_iface_t *iface,
                            const uct_obmm_device_addr_t *daddr,
                            uct_obmm_dev_info_t *info_p,
                            int *use_rx_region_p);

UCS_CLASS_DECLARE_NEW_FUNC(uct_obmm_iface_t, uct_iface_t, uct_md_h, uct_worker_h,
                           const uct_iface_params_t*, const uct_iface_config_t*);

#endif
