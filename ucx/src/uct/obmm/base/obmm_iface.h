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

#define UCT_OBMM_IFACE_FIFO_MIN_POLL_DEFAULT 16u
#define UCT_OBMM_IFACE_FIFO_MAX_POLL_DEFAULT 16u
#define UCT_OBMM_IFACE_FIFO_AI_VALUE         1u
#define UCT_OBMM_IFACE_FIFO_MD_FACTOR        2u

#define UCT_OBMM_CC_THRESH_DEFAULT          32768u
#define UCT_OBMM_CC_BUF_SIZE_DEFAULT        (2u * 1024u * 1024u)


/* Wire-format device address: identifies the obmm-side fabric coordinates
 * of the iface's owning region. Two ifaces are reachable from each other
 * iff each side has a mapped region (export OR import) carrying the
 * other's (exporter_dcna, exporter_deid). */
typedef struct uct_obmm_device_addr {
    uint64_t exporter_dcna;
    uint64_t exporter_deid_hi;
    uint64_t exporter_deid_lo;
} uct_obmm_device_addr_t;


/* Wire-format iface address: identifies the FIFO slot inside the region
 * named by the device address, plus enough geometry for the peer to
 * validate compatibility before trusting any pointer math. */
typedef struct uct_obmm_iface_addr {
    uint32_t slot_index;
    uint32_t generation;
    uint32_t pid;
    uint32_t slot_count;
    uint32_t short_lane_count;
    uint32_t fifo_size;
    uint32_t fifo_elem_size;
    uint32_t bcopy_seg_size;  /* v2: per-elem bcopy desc size; locks
                                  max_bcopy and slot_stride. v1 wrote 0
                                  here (named `reserved`); slot geometry
                                  checks prevent v1/v2 mixing. */
    uint32_t cc_enabled;      /* v3: non-zero if CC path is active */
    uint32_t cc_buf_size;     /* v3: CC buffer size in bytes        */
} uct_obmm_iface_addr_t;


typedef struct uct_obmm_iface_common_config {
    uct_iface_config_t     super;
    double                 bandwidth; /* Effective transport bandwidth in
                                         bytes/s for UCP cost modeling */
} uct_obmm_iface_common_config_t;


typedef struct uct_obmm_iface_config {
    uct_obmm_iface_common_config_t super;
    unsigned                       fifo_size;       /* FIFO ring depth (power of 2) */
    unsigned                       fifo_elem_size;  /* bytes per element (incl. hdr) */
    unsigned                       bcopy_seg_size;  /* v2: bytes per bcopy desc */
    size_t                         fifo_min_poll;   /* Minimal RX completions per progress() */
    size_t                         fifo_max_poll;   /* Maximal RX completions per progress() */
    unsigned                       pending_quota;   /* Pending retries per progress() */
    int                            cc_enable;       /* non-zero to enable CC acceleration */
    unsigned                       cc_thresh;       /* min bytes to route via CC */
    unsigned                       cc_buf_size;     /* CC buffer size (PMD_SIZE-aligned) */
} uct_obmm_iface_config_t;


typedef struct uct_obmm_iface {
    uct_base_iface_t         super;
    struct {
        double               bandwidth; /* Effective transport bandwidth in
                                           bytes/s for UCP cost modeling */
    } config;

    /* Local NC receive state -- our own slot inside the local NC export
     * region. */
    uct_obmm_pool_t          pool;            /* attached local NC export pool */
    uct_obmm_region_t       *region;          /* points into md->nc_regions[]  */
    void                    *recv_slot;       /* base of our slot bytes        */
    uct_obmm_fifo_ctl_t     *recv_ctl;        /* head/tail in our slot         */
    void                    *recv_elems;      /* fifo[] in our slot            */
    void                    *recv_descs;      /* v2: bcopy desc[] in slot      */
    uint32_t                 slot_index;      /* our slot index in pool        */
    uint32_t                 generation;      /* our slot generation token     */
    uint64_t                 read_index;      /* monotonic RX cursor           */

    /* Geometry, cached from config. fifo_size MUST be power of 2. */
    unsigned                 fifo_size;
    unsigned                 fifo_mask;       /* fifo_size - 1                 */
    unsigned                 fifo_elem_size;
    unsigned                 bcopy_seg_size;  /* v2: == max_bcopy              */
    size_t                   fifo_min_poll;
    size_t                   fifo_max_poll;
    size_t                   fifo_poll_count;
    int                      fifo_prev_wnd_cons;
    unsigned                 pending_quota;

    /* CC acceleration state.  Valid only when cc_enabled != 0. */
    int                      cc_enabled;
    unsigned                 cc_thresh;       /* threshold from config         */
    unsigned                 cc_buf_size;     /* PMD-size-aligned buffer size  */
    unsigned                 cc_num_bufs;     /* number of CC buffers          */
    uct_obmm_region_t       *cc_region;       /* local CC export, or NULL      */
    uct_obmm_region_t       *cc_peer_region;  /* peer CC import (cached from
                                                 first CC ep); or NULL        */


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

UCS_CLASS_DECLARE_NEW_FUNC(uct_obmm_iface_t, uct_iface_t, uct_md_h, uct_worker_h,
                           const uct_iface_params_t*, const uct_iface_config_t*);

#endif