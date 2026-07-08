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
} uct_obmm_md_config_t;

typedef struct uct_obmm_md {
    uct_md_t        super;
    uint64_t        local_cna;
    uct_obmm_eid_t  local_eid;
} uct_obmm_md_t;

extern ucs_config_field_t uct_obmm_md_config_table[];
extern uct_component_t uct_obmm_component;

ucs_status_t uct_obmm_md_open(uct_component_t *component, const char *md_name,
                              const uct_md_config_t *config, uct_md_h *md_p);

ucs_status_t
uct_obmm_md_discover_devices(uct_obmm_md_t *md,
                             uct_obmm_dev_info_t **devices_p,
                             unsigned *num_devices_p);

ucs_status_t
uct_obmm_md_find_device(uct_obmm_md_t *md, uint64_t exporter_dcna,
                        const uct_obmm_eid_t *exporter_deid,
                        uint32_t region_id, uct_obmm_dev_info_t *info_p);

#endif
