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
    char           *nc_memids; /* NC shmdev allow-list (was MEMIDS)  */
    char           *cc_memids; /* CC shmdev allow-list               */
} uct_obmm_md_config_t;

typedef struct uct_obmm_md {
    uct_md_t            super;

    /* NC regions: mapped with O_SYNC, used for FIFO control + data */
    uct_obmm_region_t  *nc_regions;
    unsigned            nc_num_regions;
    int                 nc_export_idx;  /* index of local NC export, or -1 */

    /* CC regions: mapped without O_SYNC, used for large payloads */
    uct_obmm_region_t  *cc_regions;
    unsigned            cc_num_regions;
    int                 cc_export_idx;  /* index of local CC export, or -1 */
} uct_obmm_md_t;

extern ucs_config_field_t uct_obmm_md_config_table[];
extern uct_component_t uct_obmm_component;

ucs_status_t uct_obmm_md_open(uct_component_t *component, const char *md_name,
                              const uct_md_config_t *config, uct_md_h *md_p);

ucs_status_t uct_obmm_md_rkey_unpack(uct_component_t *component,
                                     const void *rkey_buffer,
                                     uct_rkey_t *rkey_p, void **handle_p);

/* Look up an NC import region whose remote-side identity matches
 * (exporter_dcna, exporter_deid). Returns NULL if no such mapped region
 * exists (i.e. peer is unreachable from this MD). */
uct_obmm_region_t *
uct_obmm_md_find_import_region(uct_obmm_md_t *md, uint64_t exporter_dcna,
                               const uct_obmm_eid_t *exporter_deid);

/* Look up a CC import region matching the peer exporter identity. */
uct_obmm_region_t *
uct_obmm_md_find_cc_import_region(uct_obmm_md_t *md, uint64_t exporter_dcna,
                                  const uct_obmm_eid_t *exporter_deid);

/* Returns pointer to the locally-exported NC region, or NULL. */
uct_obmm_region_t *uct_obmm_md_export_region(uct_obmm_md_t *md);

/* Returns pointer to the locally-exported CC region, or NULL. */
uct_obmm_region_t *uct_obmm_md_cc_export_region(uct_obmm_md_t *md);

#endif