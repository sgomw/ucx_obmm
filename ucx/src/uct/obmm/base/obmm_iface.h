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

#include <uct/base/uct_iface.h>
#include <uct/sm/base/sm_iface.h>
#include <ucs/datastruct/arbiter.h>
#include <ucs/datastruct/list.h>


/* Number of slots in the per-region pool. Caps how many ifaces can attach
 * to a single 128 MiB obmm region from this host. The first iface to
 * attach to a fresh region "wins" the geometry; subsequent attaches must
 * present matching numbers. */
#define UCT_OBMM_POOL_SLOT_COUNT 256u


/* Wire-format device address: identifies the obmm-side fabric coordinates
 * of the iface's owning NC region. Keep this <=31 bytes so UCP address v1 can
 * pack it for OMPI's remote modex path. Hybrid CC identity is carried in
 * uct_obmm_iface_addr_t as a hash-validated exporter-table index. */
typedef struct uct_obmm_device_addr {
    uint64_t nc_exporter_dcna;
    uint64_t nc_exporter_deid_hi;
    uint64_t nc_exporter_deid_lo;
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
    uint32_t bcopy_seg_size;  /* v2: per-elem bcopy desc size; locks
                                 max_bcopy and slot_stride. v1 wrote 0
                                 here (named `reserved`); the pool
                                 version bump prevents v1↔v2 mixing. */
    uint32_t mode;
    uint32_t pool_version;
    uint32_t cc_chunk_size;
    uint32_t cc_chunks_per_slot;
    uint32_t cc_total_chunks;
    uint16_t cc_exporter_index;
    uint16_t reserved;
    uint64_t cc_exporters_hash;
} uct_obmm_iface_addr_t;


typedef struct uct_obmm_iface_config {
    uct_sm_iface_config_t super;
    unsigned              fifo_size;       /* FIFO ring depth (power of 2) */
    unsigned              fifo_elem_size;  /* bytes per element (incl. hdr) */
    unsigned              bcopy_seg_size;  /* v2: bytes per bcopy desc */
    size_t                cc_chunk_size;    /* hybrid: bytes per CC chunk */
    size_t                fifo_max_poll;   /* RX completions per progress() */
} uct_obmm_iface_config_t;


typedef struct uct_obmm_iface {
    uct_sm_iface_t           super;

    /* Local receive state -- our own slot inside the local export region. */
    uct_obmm_pool_t          pool;            /* attached local export pool */
    uct_obmm_region_t       *region;          /* points into md->regions[]  */
    void                    *recv_slot;       /* base of our slot bytes     */
    uct_obmm_fifo_ctl_t     *recv_ctl;        /* head/tail in our slot      */
    void                    *recv_elems;      /* fifo[] in our slot         */
    void                    *recv_descs;      /* v2: bcopy desc[] in slot   */
    uint32_t                 slot_index;      /* our slot index in pool     */
    uint32_t                 generation;      /* our slot generation token  */
    uint64_t                 read_index;      /* monotonic RX cursor        */

    /* Geometry, cached from config. fifo_size MUST be power of 2. */
    unsigned                 fifo_size;
    unsigned                 fifo_mask;       /* fifo_size - 1              */
    unsigned                 fifo_elem_size;
    unsigned                 bcopy_seg_size;  /* v2: == max_bcopy           */
    size_t                   cc_chunk_size;    /* hybrid max_bcopy          */
    size_t                   fifo_max_poll;

    uct_obmm_mem_mode_t      mode;
    uint32_t                 pool_version;

    /* Hybrid CC chunk state. The local iface owns a deterministic slice of
     * the local CC export based on slot_index; chunks are tracked by absolute
     * index within the CC export so peers can address the matching import. */
    uct_obmm_region_t       *cc_region;        /* local CC export */
    void                    *cc_slice_base;
    uint32_t                 cc_chunks_per_slot;
    uint32_t                 cc_total_chunks;
    uint16_t                 cc_first_chunk;
    uint16_t                 cc_exporter_index;
    uint16_t                *cc_free_stack;
    unsigned                 cc_free_top;
    uint64_t                 cc_exporters_hash;
    ucs_list_link_t          eps;

    /* Pending send arbiter (mirrors mm). pending_add queues UCP requests
     * here when peer FIFO is full; iface_progress dispatches them after
     * draining receives so any tail advance becomes immediately visible
     * to retries. Without this, UCP busy-spins inside ucp_do_am_bcopy_*
     * on UCS_ERR_BUSY from a no-op pending_add. */
    ucs_arbiter_t            arbiter;
} uct_obmm_iface_t;


extern ucs_config_field_t uct_obmm_iface_config_table[];

ucs_status_t
uct_obmm_iface_query_tl_devices(uct_md_h md,
                                uct_tl_device_resource_t **tl_devices_p,
                                unsigned *num_tl_devices_p);

UCS_CLASS_DECLARE_NEW_FUNC(uct_obmm_iface_t, uct_iface_t, uct_md_h, uct_worker_h,
                           const uct_iface_params_t*, const uct_iface_config_t*);

unsigned uct_obmm_iface_reclaim_all(uct_obmm_iface_t *iface);

#endif
