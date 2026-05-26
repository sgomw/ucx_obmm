/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_EP_H_
#define UCT_OBMM_EP_H_

#include "obmm_fifo.h"
#include "obmm_bulk.h"
#include "obmm_region.h"

#include <uct/base/uct_iface.h>
#include <ucs/datastruct/arbiter.h>
#include <ucs/datastruct/list.h>

typedef struct uct_obmm_iface uct_obmm_iface_t;


typedef struct uct_obmm_ep_short_lane {
    volatile uint64_t     *active_mask_p;
    uct_obmm_short_lane_t *lane;
    unsigned               lane_index;
    uint64_t               head;
    uint64_t               cached_tail;
    int                    active;
} uct_obmm_ep_short_lane_t;


typedef struct uct_obmm_ep_eager_path {
    int                      available;
    void                    *peer_slot;
    uct_obmm_fifo_ctl_t     *peer_ctl;
    void                    *peer_elems;
    void                    *peer_descs;
    uint64_t                 cached_tail;
    uint32_t                 slot_index;
    uint32_t                 generation;
    unsigned                 fifo_size;
    unsigned                 fifo_mask;
    unsigned                 fifo_elem_size;
    unsigned                 bcopy_seg_size;
    uct_obmm_ep_short_lane_t short_lane;
} uct_obmm_ep_eager_path_t;


typedef struct uct_obmm_ep_bulk_path {
    int                         available;
    void                       *peer_slot;
    uint32_t                    ctrl_slot_index;
    uint32_t                    ctrl_generation;
    uint64_t                    peer_cc_memid;
    uct_obmm_region_t          *peer_data_region;
    void                       *peer_data_base;
    uct_obmm_bulk_ctrl_hdr_t   *peer_ctrl;
    uct_obmm_bulk_window_desc_t *peer_descs;
    size_t                      window_size;
    unsigned                    window_count;
    uint64_t                    last_seen_seq;
    int                         peer_local;
} uct_obmm_ep_bulk_path_t;


typedef struct uct_obmm_ep {
    uct_base_ep_t        super;

    uint64_t                 peer_nc_dcna;
    uint64_t                 peer_nc_deid_hi;
    uint64_t                 peer_nc_deid_lo;
    uint64_t                 peer_cc_dcna;
    uint64_t                 peer_cc_deid_hi;
    uint64_t                 peer_cc_deid_lo;
    int                      is_local;
    uct_obmm_ep_eager_path_t nc;
    uct_obmm_ep_eager_path_t cc;
    uct_obmm_ep_bulk_path_t  bulk;
    ucs_list_link_t          list;

    /* Pending request queue (per ep). Scheduled on iface->arbiter from
     * pending_add when peer FIFO has no TX slot; drained by
     * uct_obmm_ep_process_pending after iface_progress publishes a new
     * tail. Mirrors mm's per-ep arb_group. */
    ucs_arbiter_group_t  arb_group;
} uct_obmm_ep_t;


UCS_CLASS_DECLARE_NEW_FUNC(uct_obmm_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DECLARE_DELETE_FUNC(uct_obmm_ep_t, uct_ep_t);

ucs_status_t uct_obmm_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t header,
                                  const void *payload, unsigned length);

ssize_t uct_obmm_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                             uct_pack_callback_t pack_cb, void *arg,
                             unsigned flags);

ucs_status_t uct_obmm_ep_pending_add(uct_ep_h tl_ep, uct_pending_req_t *n,
                                     unsigned flags);

void uct_obmm_ep_pending_purge(uct_ep_h tl_ep,
                               uct_pending_purge_callback_t cb, void *arg);

ucs_arbiter_cb_result_t
uct_obmm_ep_process_pending(ucs_arbiter_t *arbiter, ucs_arbiter_group_t *group,
                            ucs_arbiter_elem_t *elem, void *arg);

int uct_obmm_ep_is_connected(const uct_ep_h tl_ep,
                             const uct_ep_is_connected_params_t *params);

unsigned uct_obmm_ep_progress_bulk_rx(uct_obmm_ep_t *ep, uct_obmm_iface_t *iface,
                                      unsigned max_poll);
unsigned uct_obmm_iface_bulk_reclaim_windows(uct_obmm_iface_t *iface);

#endif
