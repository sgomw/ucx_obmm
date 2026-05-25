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
#include "obmm_bulk.h"

#include <stdint.h>
#include <uct/base/uct_iface.h>
#include <ucs/datastruct/arbiter.h>
#include <ucs/datastruct/list.h>

#define UCT_OBMM_IFACE_FIFO_MIN_POLL_DEFAULT 16u
#define UCT_OBMM_IFACE_FIFO_MAX_POLL_DEFAULT 16u
#define UCT_OBMM_IFACE_FIFO_AI_VALUE         1u
#define UCT_OBMM_IFACE_FIFO_MD_FACTOR        2u


/* Number of slots in the per-region pool. Caps how many ifaces can attach
 * to a single 128 MiB obmm region from this host. The first iface to
 * attach to a fresh region "wins" the geometry; subsequent attaches must
 * present matching numbers. */
#define UCT_OBMM_POOL_SLOT_COUNT 32u


typedef enum uct_obmm_iface_role {
    UCT_OBMM_IFACE_ROLE_NC_REMOTE = 0,
    UCT_OBMM_IFACE_ROLE_CC_LOCAL  = 1,
    UCT_OBMM_IFACE_ROLE_CC_BULK   = 2
} uct_obmm_iface_role_t;


static UCS_F_ALWAYS_INLINE uct_obmm_map_mode_t
uct_obmm_iface_role_map_mode(uct_obmm_iface_role_t role)
{
    return (role == UCT_OBMM_IFACE_ROLE_CC_LOCAL) ? UCT_OBMM_MAP_MODE_CC :
                                                    UCT_OBMM_MAP_MODE_NC;
}


/* Wire-format device address: identifies the obmm-side fabric coordinates
 * of the iface's owning region. Two ifaces are reachable from each other
 * iff each side has a mapped region (export OR import) carrying the
 * other's (exporter_dcna, exporter_deid). */
typedef struct uct_obmm_device_addr {
    uint64_t exporter_dcna;
    uint64_t exporter_deid_hi;
    uint64_t exporter_deid_lo;
} uct_obmm_device_addr_t;


/* Wire-format iface address: identifies the role-local slot/control entry
 * inside the region named by the device address, plus enough geometry for the
 * peer to validate compatibility before trusting any pointer math. The bulk
 * role also carries its CC memid and window geometry explicitly. */
typedef struct uct_obmm_iface_addr {
    uint32_t role;
    uint32_t slot_index;
    uint32_t generation;
    uint32_t fifo_size;
    uint32_t fifo_elem_size;
    uint32_t bcopy_seg_size;  /* v2: per-elem bcopy desc size; locks
                                  max_bcopy and slot_stride. v1 wrote 0
                                  here (named `reserved`); slot geometry
                                  checks prevent v1↔v2 mixing. */
    uint32_t bulk_window_count;
    uint32_t reserved;
    uint64_t bulk_window_size;
    uint64_t bulk_cc_memid;
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
    size_t                         bulk_window_size; /* obmm_cc_bulk only */
    unsigned                       bulk_window_count; /* obmm_cc_bulk only */
    size_t                         fifo_min_poll;   /* Minimal RX completions per progress() */
    size_t                         fifo_max_poll;   /* Maximal RX completions per progress() */
    unsigned                       pending_quota;   /* Pending retries per progress() */
} uct_obmm_iface_config_t;


typedef struct uct_obmm_iface {
    uct_base_iface_t         super;
    uct_obmm_iface_role_t    role;
    struct {
        double               bandwidth; /* Effective transport bandwidth in
                                           bytes/s for UCP cost modeling */
    } config;

    /* Role-local slot state. For eager roles this is the receive slot inside the
     * local export region; for obmm_cc_bulk it is the NC control slot. */
    uct_obmm_pool_t          pool;            /* attached local export pool */
    uct_obmm_region_t       *region;          /* points into md->regions[]  */
    void                    *recv_slot;       /* base of our slot bytes     */
    uct_obmm_fifo_ctl_t     *recv_ctl;        /* head/tail in our slot      */
    volatile uint64_t       *recv_short_active_mask; /* active SPSC lanes */
    uct_obmm_short_lane_t   *recv_short_lanes; /* deterministic small-msg lanes */
    unsigned                 recv_short_hot_lane; /* last lane that produced RX */
    void                    *recv_elems;      /* fifo[] in our slot         */
    void                    *recv_descs;      /* v2: bcopy desc[] in slot   */
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
    unsigned                 bcopy_seg_size;  /* v2: == max_bcopy           */
    size_t                   fifo_min_poll;
    size_t                   fifo_max_poll;
    size_t                   fifo_poll_count;
    int                      fifo_prev_wnd_cons;
    unsigned                 pending_quota;
    ucs_list_link_t          ep_list;

    /* Pending send arbiter (mirrors mm). pending_add queues UCP requests
     * when peer FIFO state still looks full after a normal tail refresh;
     * iface_progress dispatches them after draining receives so newly
     * published tails become visible to retries. */
    ucs_arbiter_t            arbiter;

    /* obmm_cc_bulk data path: NC control slot plus CC export windows after the
     * fixed 32 MiB local-eager prefix. */
    uct_obmm_region_t       *data_region;
    void                    *bulk_data_base;
    uct_obmm_bulk_ctrl_hdr_t *bulk_ctrl_hdr;
    uct_obmm_bulk_window_desc_t *bulk_ctrl_descs;
    size_t                   bulk_data_offset;
    size_t                   bulk_window_size;
    unsigned                 bulk_window_count;
    unsigned                 bulk_next_window;
} uct_obmm_iface_t;


extern ucs_config_field_t uct_obmm_iface_config_table[];

ucs_status_t
uct_obmm_iface_query_tl_devices(uct_md_h md,
                                uct_tl_device_resource_t **tl_devices_p,
                                unsigned *num_tl_devices_p);

ucs_status_t
uct_obmm_cc_iface_query_tl_devices(uct_md_h md,
                                   uct_tl_device_resource_t **tl_devices_p,
                                   unsigned *num_tl_devices_p);
ucs_status_t
uct_obmm_cc_bulk_iface_query_tl_devices(uct_md_h md,
                                        uct_tl_device_resource_t **tl_devices_p,
                                        unsigned *num_tl_devices_p);

UCS_CLASS_DECLARE_NEW_FUNC(uct_obmm_iface_t, uct_iface_t, uct_md_h, uct_worker_h,
                           const uct_iface_params_t*, const uct_iface_config_t*);

#endif
