/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_MD_H_
#define UCT_OBMM_MD_H_

#include "obmm_region.h"
#include "obmm_sysfs.h"
#include "obmm_pool.h"

#include <uct/base/uct_md.h>


typedef struct uct_obmm_md_config {
    uct_md_config_t super;
    char           *mem_mode;
    char           *nc_memids;
    char           *cc_memids;
} uct_obmm_md_config_t;

typedef struct uct_obmm_exporter_id {
    uint64_t       dcna;
    uct_obmm_eid_t deid;
} uct_obmm_exporter_id_t;

typedef struct uct_obmm_md {
    uct_md_t               super;
    uct_obmm_mem_mode_t    mode;

    uct_obmm_region_t     *nc_regions;
    unsigned               num_nc_regions;
    int                    nc_export_idx;

    uct_obmm_region_t     *cc_regions;
    unsigned               num_cc_regions;
    int                    cc_export_idx;

    uct_obmm_exporter_id_t *cc_exporters;
    unsigned               num_cc_exporters;
    uint64_t               cc_exporters_hash;
    uint16_t               cc_exporter_index;
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
uct_obmm_md_find_import_region(uct_obmm_md_t *md, uint64_t exporter_dcna,
                               const uct_obmm_eid_t *exporter_deid);

uct_obmm_region_t *
uct_obmm_md_find_cc_region(uct_obmm_md_t *md, uint64_t exporter_dcna,
                           const uct_obmm_eid_t *exporter_deid);

uct_obmm_region_t *
uct_obmm_md_find_cc_region_by_index(uct_obmm_md_t *md, uint16_t index);

ucs_status_t uct_obmm_md_cc_exporter_index(uct_obmm_md_t *md,
                                           uint64_t exporter_dcna,
                                           const uct_obmm_eid_t *exporter_deid,
                                           uint16_t *index_p);

/* Returns pointer to the locally-exported region we initialize our pool in,
 * or NULL if this MD has no local export. */
uct_obmm_region_t *uct_obmm_md_export_region(uct_obmm_md_t *md);

uct_obmm_region_t *uct_obmm_md_cc_export_region(uct_obmm_md_t *md);

#endif
