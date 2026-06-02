/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_EP_H_
#define UCT_OBMM_EP_H_

#include "obmm_fifo.h"
#include "obmm_region.h"

#include <uct/base/uct_iface.h>
#include <ucs/datastruct/arbiter.h>


typedef struct uct_obmm_ep {
    uct_base_ep_t        super;

    /* Peer FIFO state. Pointers refer into the MD-owned mapping of the
     * peer's NC region (either local NC export for self-loopback, or one
     * of the NC imports). The MD outlives all ifaces/eps, so these
     * pointers remain valid until ep destroy. */
    uct_obmm_fifo_ctl_t *peer_ctl;
    void                *peer_elems;
    void                *peer_descs;     /* v2: bcopy desc array          */
    uint64_t             cached_tail;

    /* Stamped into every outgoing element so the receiver can drop stale
     * writes after slot reuse. */
    uint32_t             expected_generation;

    /* Peer NC geometry (mirrored from remote iface_addr; pre-validated to
     * match our own at ep create time). */
    unsigned             fifo_size;
    unsigned             fifo_mask;
    unsigned             fifo_elem_size;
    unsigned             bcopy_seg_size;

    /* Peer CC geometry (from iface_addr cc_enabled, cc_buf_size). */
    unsigned             cc_enabled;
    unsigned             cc_buf_size;
    unsigned             cc_num_bufs;

    /* Peer CC import region.  Points into md->cc_regions[].  This is
     * the local mapping of the peer's CC export; the receiver reads
     * CC payload data from here using obmm_set_ownership.  Valid only
     * when cc_enabled is non-zero. */
    uct_obmm_region_t   *cc_peer_region;
    int                  cc_peer_fd;

    /* Identity (cached from remote iface_addr/device_addr for diagnostics
     * and is_connected checks). */
    uint64_t             peer_dcna;
    uint64_t             peer_deid_hi;
    uint64_t             peer_deid_lo;
    uint32_t             peer_slot_index;
    uint32_t             peer_pid;

    /* Pending request queue (per ep). */
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

#endif