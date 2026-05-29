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
#include <ucs/debug/log.h>
#include <ucs/datastruct/arbiter.h>
#include <ucs/datastruct/list.h>

#define UCT_OBMM_IFACE_FIFO_MIN_POLL_DEFAULT 16u
#define UCT_OBMM_IFACE_FIFO_MAX_POLL_DEFAULT 16u
#define UCT_OBMM_IFACE_FIFO_AI_VALUE         1u
#define UCT_OBMM_IFACE_FIFO_MD_FACTOR        2u


enum {
    UCT_OBMM_IFACE_ADDR_FLAG_CC_EAGER = UCS_BIT(0),
    UCT_OBMM_IFACE_ADDR_FLAG_BULK     = UCS_BIT(1)
};


/* Wire-format device address: NC reachability and same-node locality only need
 * the shared exporter identity for the peer process. */
typedef struct uct_obmm_device_addr {
    uint64_t exporter_dcna;
    uint64_t exporter_deid_hi;
    uint64_t exporter_deid_lo;
} UCS_S_PACKED uct_obmm_device_addr_t;


typedef struct uct_obmm_slot_addr {
    uint32_t generation;
    uint16_t fifo_size;
    uint16_t fifo_elem_size;
    uint16_t bcopy_seg_size;
    uint8_t  slot_index;
} UCS_S_PACKED uct_obmm_slot_addr_t;


/* obmm_cc keeps a dedicated compact iface address so the fixed same-node eager
 * segment can exceed uint16 without bloating obmm_nc's worker address past the
 * legacy UCP v1 packing limit. */
typedef struct uct_obmm_cc_iface_addr {
    uint32_t generation;
    uint32_t bcopy_seg_size;
    uint16_t fifo_size;
    uint16_t fifo_elem_size;
    uint8_t  slot_index;
} UCS_S_PACKED uct_obmm_cc_iface_addr_t;


typedef struct uct_obmm_cc_addr {
    uint64_t exporter_dcna;
    uint64_t exporter_deid_hi;
    uint64_t exporter_deid_lo;
    uint32_t generation;
    uint8_t  slot_index;
} UCS_S_PACKED uct_obmm_cc_addr_t;


/* Wire-format iface address for obmm_nc. It carries the cross-node eager
 * geometry plus the CC/bulk metadata needed for the internal bulk path. */
typedef struct uct_obmm_iface_addr {
    uint8_t              flags;
    uct_obmm_slot_addr_t nc;
    uct_obmm_cc_addr_t   cc;
    uint32_t             bulk_ctrl_generation;
    uint32_t             bulk_data_offset;
    uint32_t             bulk_window_size;
    uint64_t             bulk_cc_memid;
    uint8_t              bulk_ctrl_slot_index;
    uint8_t              bulk_window_count;
} UCS_S_PACKED uct_obmm_iface_addr_t;

#if UCT_OBMM_POOL_SLOT_COUNT > UINT8_MAX
#error "obmm wire format requires slot_count <= UINT8_MAX"
#endif

#if UCT_OBMM_CC_LOCAL_FIFO_SIZE > UINT16_MAX
#error "obmm wire format requires CC fifo_size <= UINT16_MAX"
#endif

#if UCT_OBMM_CC_LOCAL_FIFO_ELEM_SIZE > UINT16_MAX
#error "obmm wire format requires CC fifo_elem_size <= UINT16_MAX"
#endif

#if UCT_OBMM_CC_LOCAL_BCOPY_SEG_SIZE > UINT32_MAX
#error "obmm wire format requires CC bcopy_seg_size <= UINT32_MAX"
#endif

typedef char uct_obmm_device_addr_v1_size_check[
        (sizeof(uct_obmm_device_addr_t) <= 31) ? 1 : -1];
typedef char uct_obmm_cc_iface_addr_v1_size_check[
        (sizeof(uct_obmm_cc_iface_addr_t) <= 63) ? 1 : -1];
typedef char uct_obmm_iface_addr_v1_size_check[
        (sizeof(uct_obmm_iface_addr_t) <= 63) ? 1 : -1];


typedef struct uct_obmm_iface_common_config {
    uct_iface_config_t     super;
    double                 bandwidth; /* Effective transport bandwidth in
                                         bytes/s for UCP cost modeling */
} uct_obmm_iface_common_config_t;


typedef enum uct_obmm_iface_role {
    UCT_OBMM_IFACE_ROLE_NC = 0,
    UCT_OBMM_IFACE_ROLE_CC = 1
} uct_obmm_iface_role_t;


typedef struct uct_obmm_iface_config {
    uct_obmm_iface_common_config_t super;
    unsigned                       fifo_size;       /* FIFO ring depth (power of 2) */
    unsigned                       fifo_elem_size;  /* bytes per element (incl. hdr) */
    unsigned                       bcopy_seg_size;  /* bytes per NC eager bcopy desc */
    size_t                         bulk_window_size; /* bytes per CC bulk window */
    unsigned                       bulk_window_count; /* CC bulk windows per iface */
    size_t                         fifo_min_poll;   /* Minimal RX completions per progress() */
    size_t                         fifo_max_poll;   /* Maximal RX completions per progress() */
    unsigned                       pending_quota;   /* Pending retries per progress() */
    int                            debug_log;       /* Emit compact diagnostic warnings */
} uct_obmm_iface_config_t;


typedef struct uct_obmm_iface_eager_path {
    int                      available;
    uct_obmm_region_t       *region;
    uct_obmm_pool_t          pool;
    void                    *recv_slot;
    uct_obmm_fifo_ctl_t     *recv_ctl;
    volatile uint64_t       *recv_short_active_mask;
    uct_obmm_short_lane_t   *recv_short_lanes;
    unsigned                 recv_short_hot_lane;
    void                    *recv_elems;
    void                    *recv_descs;
    uint32_t                 slot_index;
    uint32_t                 generation;
    uint64_t                 read_index;
    uint64_t                 recv_published_tail;
    uint64_t                 recv_short_tails[UCT_OBMM_SHORT_LANE_COUNT];
    uint64_t                 recv_short_published_tails[UCT_OBMM_SHORT_LANE_COUNT];
    unsigned                 recv_tail_batch;
    unsigned                 recv_free_desc_count;
    uint32_t                 recv_free_descs[
            UCT_OBMM_CC_LOCAL_DESC_COUNT - UCT_OBMM_CC_LOCAL_FIFO_SIZE];
    size_t                   rx_headroom;
    unsigned                 fifo_size;
    unsigned                 fifo_mask;
    unsigned                 fifo_elem_size;
    unsigned                 bcopy_seg_size;
    uct_recv_desc_t          release_desc;
} uct_obmm_iface_eager_path_t;


typedef struct uct_obmm_iface_bulk_path {
    int                         available;
    void                       *ctrl_slot;
    uint32_t                    ctrl_slot_index;
    uint32_t                    ctrl_generation;
    uct_obmm_bulk_ctrl_hdr_t   *ctrl_hdr;
    uct_obmm_bulk_window_desc_t *ctrl_descs;
    uct_obmm_region_t          *data_region;
    void                       *data_base;
    size_t                      data_offset;
    size_t                      window_size;
    unsigned                    window_count;
    unsigned                    next_window;
    unsigned                    inflight;
    unsigned                    idle_polls;
} uct_obmm_iface_bulk_path_t;


typedef struct uct_obmm_iface {
    uct_base_iface_t         super;
    struct {
        double               bandwidth; /* Effective transport bandwidth in
                                           bytes/s for UCP cost modeling */
    } config;
    uct_obmm_iface_role_t    role;

    uct_obmm_region_t       *nc_region;
    uct_obmm_region_t       *cc_region;
    uct_obmm_iface_eager_path_t nc;
    uct_obmm_iface_eager_path_t cc;
    uct_obmm_iface_bulk_path_t  bulk;
    uint8_t                  short_copy_buf[UCT_OBMM_SHORT_LANE_ELEM_SIZE];
    size_t                   fifo_min_poll;
    size_t                   fifo_max_poll;
    size_t                   fifo_poll_count;
    int                      fifo_prev_wnd_cons;
    unsigned                 pending_quota;
    int                      debug_log;
    uint64_t                 debug_tx_eager_count;
    uint64_t                 debug_rx_eager_count;
    uint64_t                 debug_tail_count;
    uint64_t                 debug_path_count;
    uint64_t                 debug_tx_bulk_count;
    uint64_t                 debug_rx_bulk_count;
    uint64_t                 debug_reclaim_count;
    uint64_t                 debug_nores_count;
    uint64_t                 debug_pending_count;
    ucs_list_link_t          ep_list;

    /* Pending send arbiter (mirrors mm). pending_add queues UCP requests
     * when peer FIFO state still looks full after a normal tail refresh;
     * iface_progress dispatches them after draining receives so newly
     * published tails become visible to retries. */
    ucs_arbiter_t            arbiter;

} uct_obmm_iface_t;


#define UCT_OBMM_DBG(_iface, _fmt, ...) \
    do { \
        if (ucs_unlikely((_iface)->debug_log)) { \
            ucs_warn("obmmD " _fmt, ## __VA_ARGS__); \
        } \
    } while (0)


static UCS_F_ALWAYS_INLINE int
uct_obmm_debug_should_log(uint64_t *counter)
{
    uint64_t value = ++(*counter);

    return (value <= 8) || ((value & (value - 1)) == 0);
}


extern ucs_config_field_t uct_obmm_iface_config_table[];

UCS_CLASS_DECLARE_NEW_FUNC(uct_obmm_iface_t, uct_iface_t, uct_md_h, uct_worker_h,
                           const uct_iface_params_t*, const uct_iface_config_t*);

#endif
