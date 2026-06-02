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


/* Number of slots in the per-region pool. Caps how many ifaces can attach
 * to a single 256 MiB obmm region from this host. The first iface to
 * attach to a fresh region "wins" the geometry; subsequent attaches must
 * present matching numbers.
 *
 * IMPORTANT: UCT_OBMM_SHORT_LANE_COUNT (obmm_fifo.h) must equal
 * 2 * POOL_SLOT_COUNT — lane 0..slot_count-1 routes to local senders,
 * lane slot_count..2*slot_count-1 routes to import-side senders. */
#define UCT_OBMM_POOL_SLOT_COUNT 100u

/* Validate the coupling between SHORT_LANE_COUNT (obmm_fifo.h) and
 * POOL_SLOT_COUNT at compile time. Using #if/#error rather than
 * UCS_STATIC_ASSERT because both constants are preprocessor defines
 * and this avoids the switch-statement static-assert pattern. */
#if UCT_OBMM_SHORT_LANE_COUNT != (2u * UCT_OBMM_POOL_SLOT_COUNT)
#error "UCT_OBMM_SHORT_LANE_COUNT must equal 2 * UCT_OBMM_POOL_SLOT_COUNT"
#endif


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
    uint32_t fifo_size;
    uint32_t fifo_elem_size;
    uint32_t bcopy_seg_size;  /* per-elem bcopy desc size; locks max_bcopy
                                 and slot_stride. replaced legacy `reserved`
                                 u32 — no struct-size change. */
} uct_obmm_iface_addr_t;


typedef struct uct_obmm_iface_config {
    uct_iface_config_t super;
    unsigned           fifo_size;       /* FIFO ring depth (power of 2) */
    unsigned           fifo_elem_size;  /* bytes per element (incl. hdr) */
    unsigned           bcopy_seg_size;  /* bytes per bcopy desc */
    unsigned           pending_quota;   /* pending retries per progress() */
} uct_obmm_iface_config_t;


/* Fixed progress budget. One progress() call drains up to this many
 * receive completions before yielding. */
#define UCT_OBMM_IFACE_PROGRESS_BUDGET 16u


typedef struct uct_obmm_iface {
    uct_base_iface_t         super;

    /* Local receive state — our own slot inside the local export region. */
    uct_obmm_pool_t          pool;            /* attached local export pool */
    uct_obmm_region_t       *region;          /* points into md->regions[]  */
    void                    *recv_slot;       /* base of our slot bytes     */
    uct_obmm_fifo_ctl_t     *recv_ctl;        /* head/tail in our slot      */
    volatile uint64_t       *recv_short_active_mask; /* active SPSC lanes */
    uct_obmm_short_lane_t   *recv_short_lanes; /* deterministic small-msg lanes */
    unsigned                 recv_short_hot_lane;  /* last lane that produced RX */
    uint64_t                 recv_short_last_db;   /* doorbell from last scan */
    void                    *recv_elems;      /* fifo[] in our slot         */
    void                    *recv_descs;      /* bcopy desc[] in our slot   */
    uint32_t                 slot_index;      /* our slot index in pool     */
    uint32_t                 generation;      /* our slot generation token  */
    uint64_t                 read_index;      /* monotonic RX cursor        */
    uint64_t                 recv_short_tails[UCT_OBMM_SHORT_LANE_COUNT];
    uint64_t                 recv_short_published_tails[UCT_OBMM_SHORT_LANE_COUNT];
    uint8_t                  short_copy_buf[UCT_OBMM_SHORT_LANE_ELEM_SIZE];

    /* Geometry, cached from config. fifo_size MUST be power of 2. */
    unsigned                 fifo_size;
    unsigned                 fifo_mask;       /* fifo_size - 1              */
    unsigned                 fifo_elem_size;
    unsigned                 bcopy_seg_size;  /* == max_bcopy               */
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

UCS_CLASS_DECLARE_NEW_FUNC(uct_obmm_iface_t, uct_iface_t, uct_md_h, uct_worker_h,
                           const uct_iface_params_t*, const uct_iface_config_t*);

#endif
