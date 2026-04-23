/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_MD_H_
#define UCT_OBMM_MD_H_

#include "obmm_region.h"

#include <uct/base/uct_md.h>


typedef struct uct_obmm_md_config {
    uct_md_config_t super;
} uct_obmm_md_config_t;

typedef struct uct_obmm_md {
    uct_md_t            super;
    uct_obmm_region_t  *regions;     /* mapped shmdev regions       */
    unsigned            num_regions;
    int                 export_idx;  /* index of local export, or -1 */
} uct_obmm_md_t;

extern ucs_config_field_t uct_obmm_md_config_table[];
extern uct_component_t uct_obmm_component;

ucs_status_t uct_obmm_md_open(uct_component_t *component, const char *md_name,
                              const uct_md_config_t *config, uct_md_h *md_p);

ucs_status_t uct_obmm_md_rkey_unpack(uct_component_t *component,
                                     const void *rkey_buffer,
                                     uct_rkey_t *rkey_p, void **handle_p);

#endif
