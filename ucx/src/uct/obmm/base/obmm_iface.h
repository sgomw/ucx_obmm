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

#include <stdint.h>
#include <uct/base/uct_iface.h>
#include <ucs/datastruct/arbiter.h>

#define UCT_OBMM_IFACE_FIFO_SIZE_DEFAULT     256
#define UCT_OBMM_IFACE_FIFO_MIN_POLL_DEFAULT 64
#define UCT_OBMM_IFACE_FIFO_MAX_POLL_DEFAULT 128
#define UCT_OBMM_IFACE_FIFO_AI_VALUE         1u
#define UCT_OBMM_IFACE_FIFO_MD_FACTOR        2u


struct uct_obmm_ep;


typedef struct uct_obmm_region_addr {
    uint64_t exporter_dcna;
    uint64_t exporter_deid_hi;
    uint64_t exporter_deid_lo;
    uint32_t region_id;
} UCS_S_PACKED uct_obmm_region_addr_t;


/* Wire-format device address. Keep this at 31 bytes or less so UCP's default
 * worker-address v1 format can pack it. `primary` identifies the claimed NC
 * export block. */
typedef struct uct_obmm_device_addr {
    uct_obmm_region_addr_t primary;
} UCS_S_PACKED uct_obmm_device_addr_t;


/* Wire-format iface address. UCP worker-address v1 allows up to 63 bytes here.
 * Slot index is fixed at 0 for the current one-FIFO-per-export-block layout. */
typedef struct uct_obmm_iface_addr {
    uint32_t slot_index;
    uint32_t pid;
    uint32_t wire_format;
    uint32_t fifo_size;
    uint32_t fifo_elem_size;
    uint32_t bcopy_seg_size;  /* advertised max_bcopy; payload must fit in
                                  the shared FIFO element data area. */
} UCS_S_PACKED uct_obmm_iface_addr_t;

typedef struct uct_obmm_iface_common_config {
    uct_iface_config_t     super;
    double                 bandwidth; /* Effective transport bandwidth in
                                         bytes/s for UCP cost modeling */
} uct_obmm_iface_common_config_t;


typedef struct uct_obmm_iface_config {
    uct_obmm_iface_common_config_t super;
    unsigned                       fifo_size;       /* FIFO ring depth (power of 2) */
    unsigned                       fifo_elem_size;  /* bytes per element (incl. hdr) */
    unsigned                       bcopy_seg_size;  /* bytes per bcopy desc */
    double                         short_overhead;  /* AM_SHORT per-side model */
    double                         bcopy_overhead;  /* AM_BCOPY per-side model */
    size_t                         fifo_min_poll;   /* Minimal RX completions per progress() */
    size_t                         fifo_max_poll;   /* Maximal RX completions per progress() */
    unsigned                       pending_quota;   /* Pending retries per progress() */
} uct_obmm_iface_config_t;


typedef struct uct_obmm_iface_rx {
    uct_obmm_pool_t          pool;
    uct_obmm_region_t        region_storage;
    uct_obmm_region_t       *region;
    void                    *recv_slot;
    uct_obmm_fifo_ctl_t     *recv_ctl;
    void                    *recv_elems;
    uint32_t                 slot_index;
    uint64_t                 read_index;
    size_t                   fifo_poll_count;
    int                      fifo_prev_wnd_cons;
    int                      region_opened;
    int                      active;
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
    size_t                   fifo_min_poll;
    size_t                   fifo_max_poll;
    unsigned                 pending_quota;

    /* Pending send arbiter (mirrors mm). pending_add queues UCP requests
     * when peer FIFO state still looks full after a normal tail refresh;
     * iface_progress dispatches them after draining receives so newly
     * published tails become visible to retries. */
    ucs_arbiter_t            arbiter;

    int                      base_initialized;
    int                      arbiter_initialized;
} uct_obmm_iface_t;


extern ucs_config_field_t uct_obmm_iface_config_table[];

ucs_status_t
uct_obmm_iface_query_tl_devices(uct_md_h md,
                                uct_tl_device_resource_t **tl_devices_p,
                                unsigned *num_tl_devices_p);

ucs_status_t
uct_obmm_iface_resolve_peer(uct_obmm_iface_t *iface,
                            const uct_obmm_device_addr_t *daddr,
                            const uct_obmm_iface_addr_t *iaddr,
                            uct_obmm_dev_info_t *info_p,
                            uint32_t *slot_index_p,
                            int *use_rx_region_p);

UCS_CLASS_DECLARE_NEW_FUNC(uct_obmm_iface_t, uct_iface_t, uct_md_h, uct_worker_h,
                           const uct_iface_params_t*, const uct_iface_config_t*);

#endif
