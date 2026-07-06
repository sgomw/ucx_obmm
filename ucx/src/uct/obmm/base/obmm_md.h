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
    char           *memids;       /* optional shmdev memid CSV allow-list */
} uct_obmm_md_config_t;

typedef struct uct_obmm_md {
    uct_md_t            super;
    uct_obmm_region_t  *regions;     /* mapped shmdev regions       */
    unsigned            num_regions;
    unsigned            num_exports;
} uct_obmm_md_t;

extern ucs_config_field_t uct_obmm_md_config_table[];
extern uct_component_t uct_obmm_component;

ucs_status_t uct_obmm_md_open(uct_component_t *component, const char *md_name,
                              const uct_md_config_t *config, uct_md_h *md_p);

/* Look up any mapped region, export or import, whose exporter identity matches
 * (exporter_dcna, exporter_deid). */
uct_obmm_region_t *
uct_obmm_md_find_region(uct_obmm_md_t *md, uint64_t exporter_dcna,
                        const uct_obmm_eid_t *exporter_deid,
                        uint32_t region_id);

/* Look up an import region whose remote-side identity matches
 * (exporter_dcna, exporter_deid). Returns NULL if no such mapped import
 * exists. */
uct_obmm_region_t *
uct_obmm_md_find_import_region(uct_obmm_md_t *md, uint64_t exporter_dcna,
                               const uct_obmm_eid_t *exporter_deid,
                               uint32_t region_id);

unsigned uct_obmm_md_num_export_regions(uct_obmm_md_t *md);

/* Returns the @a index'th locally-exported region, or NULL if it does not
 * exist. */
uct_obmm_region_t *uct_obmm_md_export_region(uct_obmm_md_t *md,
                                             unsigned index);

#endif
