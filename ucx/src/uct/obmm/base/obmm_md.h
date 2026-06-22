/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_MD_H_
#define UCT_OBMM_MD_H_

#include "obmm_region.h"
#include "obmm_sysfs.h"

#include <uct/base/uct_md.h>


typedef struct uct_obmm_md_config {
    uct_md_config_t super;
    char           *nc_memids;  /* optional explicit NC CSV allow-list */
    char           *cc_memids;  /* optional explicit same-node CC CSV list */
} uct_obmm_md_config_t;

typedef struct uct_obmm_md {
    uct_md_t            super;
    uct_obmm_region_t  *regions;     /* mapped shmdev regions       */
    unsigned            num_regions;
    int                 export_idx[UCT_OBMM_PLANE_LAST]; /* per-plane export */
} uct_obmm_md_t;

extern ucs_config_field_t uct_obmm_md_config_table[];
extern uct_component_t uct_obmm_component;

ucs_status_t uct_obmm_md_open(uct_component_t *component, const char *md_name,
                              const uct_md_config_t *config, uct_md_h *md_p);

ucs_status_t uct_obmm_md_rkey_unpack(uct_component_t *component,
                                     const void *rkey_buffer,
                                     uct_rkey_t *rkey_p, void **handle_p);

/* Look up any mapped region, export or import, whose exporter identity matches
 * (exporter_dcna, exporter_deid). */
uct_obmm_region_t *
uct_obmm_md_find_region(uct_obmm_md_t *md, uct_obmm_plane_t plane,
                        uint64_t exporter_dcna,
                        const uct_obmm_eid_t *exporter_deid);

/* Look up an import region whose remote-side identity matches
 * (exporter_dcna, exporter_deid). Returns NULL if no such mapped import
 * exists. */
uct_obmm_region_t *
uct_obmm_md_find_import_region(uct_obmm_md_t *md, uct_obmm_plane_t plane,
                               uint64_t exporter_dcna,
                               const uct_obmm_eid_t *exporter_deid);

/* Returns pointer to the locally-exported region we initialize our pool in,
 * or NULL if this MD has no local export. */
uct_obmm_region_t *uct_obmm_md_export_region(uct_obmm_md_t *md,
                                             uct_obmm_plane_t plane);

int uct_obmm_md_has_plane(uct_obmm_md_t *md, uct_obmm_plane_t plane);

/* Standalone obmm_nc fallback: when no local CC export is configured, NC may
 * also use the local NC export for same-node peers. In the dual-plane mode,
 * CC owns same-node traffic and NC stays cross-node/import-only. */
int uct_obmm_md_allow_nc_local_loopback(uct_obmm_md_t *md);

#endif
