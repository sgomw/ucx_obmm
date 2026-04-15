/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_IFACE_H_
#define UCT_OBMM_IFACE_H_

#include "obmm_md.h"

#include <uct/base/uct_iface.h>
#include <uct/sm/base/sm_iface.h>


typedef struct uct_obmm_iface_addr {
    uint16_t version;
    uint16_t device_index;
    uint32_t md_generation;
    uint64_t iface_uuid;
} UCS_S_PACKED uct_obmm_iface_addr_t;

typedef struct uct_obmm_iface_config {
    uct_sm_iface_config_t super;
    double                latency;
    double                overhead;
    unsigned              priority;
} uct_obmm_iface_config_t;

typedef struct uct_obmm_iface {
    uct_sm_iface_t     super;
    uint64_t           iface_uuid;
    uct_obmm_device_t *device;
    uint32_t           md_generation;
    double             overhead;
    unsigned           priority;
    ucs_linear_func_t  latency;
} uct_obmm_iface_t;

extern ucs_config_field_t uct_obmm_iface_config_table[];

ucs_status_t
uct_obmm_iface_query_tl_devices(uct_md_h md,
                                uct_tl_device_resource_t **tl_devices_p,
                                unsigned *num_tl_devices_p);

UCS_CLASS_DECLARE_NEW_FUNC(uct_obmm_iface_t, uct_iface_t, uct_md_h, uct_worker_h,
                           const uct_iface_params_t*, const uct_iface_config_t*);

#endif
