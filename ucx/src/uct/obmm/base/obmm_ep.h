/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_EP_H_
#define UCT_OBMM_EP_H_

#include "obmm_fifo.h"

#include <uct/base/uct_iface.h>
#include <ucs/datastruct/arbiter.h>


typedef struct uct_obmm_ep {
    uct_base_ep_t           super;

    /* Outbound MPSC receiver-local shard in the peer receiver slot. */
    uct_obmm_fifo_ctl_t    *fifo_ctl;
    void                   *fifo_elems;
    void                   *fifo_descs;
    uint64_t                cached_tail;

    /* Peer identity (mirrored from remote iface_addr/device_addr). */
    uint32_t                expected_generation; /* peer slot generation */
    uint64_t                peer_dcna;
    uint64_t                peer_deid_hi;
    uint64_t                peer_deid_lo;
    uint32_t                peer_slot_index;
    uint32_t                peer_pid;
    uint32_t                shard_count;
    uint32_t                shard_index;
    uint8_t                 fifo_bank;

    /* Per-ep geometry mirrors the peer iface geometry after validation. */
    unsigned                fifo_size;
    unsigned                fifo_mask;
    unsigned                fifo_elem_size;
    unsigned                bcopy_seg_size;

    /* Pending request queue (per ep). */
    ucs_arbiter_group_t     arb_group;
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

#endif
