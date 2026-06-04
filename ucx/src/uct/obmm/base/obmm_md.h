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
    char           *memids;     /* optional legacy NC CSV allow-list */
    char           *nc_memids;  /* optional explicit NC CSV allow-list */
    char           *cc_memids;  /* optional explicit CC CSV allow-list */
} uct_obmm_md_config_t;

typedef struct uct_obmm_md {
    uct_md_t            super;
    uct_obmm_region_t  *regions;     /* mapped shmdev regions       */
    unsigned            num_regions;
    int                 nc_export_idx; /* local NC export, or -1      */
    int                 cc_export_idx; /* local CC export, or -1      */
} uct_obmm_md_t;

extern ucs_config_field_t uct_obmm_md_config_table[];
extern uct_component_t uct_obmm_component;

ucs_status_t uct_obmm_md_open(uct_component_t *component, const char *md_name,
                              const uct_md_config_t *config, uct_md_h *md_p);

ucs_status_t uct_obmm_md_rkey_unpack(uct_component_t *component,
                                     const void *rkey_buffer,
                                     uct_rkey_t *rkey_p, void **handle_p);

/* Look up an import region whose remote-side identity matches
 * (exporter_dcna, exporter_deid). Returns NULL if no such mapped region
 * exists (i.e. peer is unreachable from this MD). */
uct_obmm_region_t *
uct_obmm_md_find_region(uct_obmm_md_t *md, uct_obmm_region_kind_t kind,
                        uint64_t exporter_dcna,
                        const uct_obmm_eid_t *exporter_deid);

uct_obmm_region_t *
uct_obmm_md_find_import_region(uct_obmm_md_t *md, uct_obmm_region_kind_t kind,
                               uint64_t exporter_dcna,
                               const uct_obmm_eid_t *exporter_deid);

/* Returns pointer to the locally-exported region we initialize our pool in,
 * or NULL if this MD has no local export. */
uct_obmm_region_t *uct_obmm_md_export_region(uct_obmm_md_t *md,
                                             uct_obmm_region_kind_t kind);

#endif
