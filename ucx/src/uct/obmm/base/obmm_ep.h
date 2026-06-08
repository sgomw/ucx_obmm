/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_EP_H_
#define UCT_OBMM_EP_H_

#include "obmm_fifo.h"
#include "obmm_iface.h"

#include <uct/base/uct_iface.h>
#include <ucs/datastruct/arbiter.h>

typedef struct uct_obmm_ep {
    uct_base_ep_t        super;

    /* Peer FIFO state. Pointers refer into the MD-owned mapping of the
     * peer's region (either local export for self-loopback, or one of the
     * imports). The MD outlives all ifaces/eps, so these pointers remain
     * valid until ep destroy. */
    uct_obmm_fifo_ctl_t *peer_ctl;
    void                *peer_elems;
    uint64_t             cached_tail;

    /* Stamped into every outgoing element so the receiver can drop stale
     * writes after slot reuse. */
    uint32_t             expected_generation;
    uct_obmm_plane_t     plane;

    /* Peer geometry (mirrored from remote iface_addr; pre-validated to
     * match our own at ep create time). */
    unsigned             fifo_size;
    unsigned             fifo_mask;
    unsigned             fifo_elem_size;
    unsigned             bcopy_seg_size;

    /* Identity (cached from remote iface_addr/device_addr for diagnostics
     * and is_connected checks). */
    uint64_t             peer_dcna;
    uint64_t             peer_deid_hi;
    uint64_t             peer_deid_lo;
    uint32_t             peer_slot_index;
    uint32_t             peer_pid;

    /* Pending request queue (per ep). Scheduled on iface->arbiter from
     * pending_add when peer FIFO has no TX slot; drained by
     * uct_obmm_ep_process_pending after iface_progress publishes a new
     * tail. Mirrors mm's per-ep arb_group. */
    ucs_arbiter_group_t  arb_group;
    int                  base_initialized;
    int                  arb_group_initialized;
} uct_obmm_ep_t;


UCS_CLASS_DECLARE_NEW_FUNC(uct_obmm_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DECLARE_DELETE_FUNC(uct_obmm_ep_t, uct_ep_t);

ucs_status_t uct_obmm_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t header,
                                  const void *payload, unsigned length);

ssize_t uct_obmm_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                             uct_pack_callback_t pack_cb, void *arg,
                             unsigned flags);

ucs_status_t uct_obmm_ep_flush(uct_ep_h tl_ep, unsigned flags,
                               uct_completion_t *comp);

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
