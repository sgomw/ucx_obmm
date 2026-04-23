/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "obmm_md.h"
#include "obmm_sysfs.h"

#include <ucs/debug/log.h>
#include <ucs/debug/memtrack_int.h>
#include <uct/sm/base/sm_md.h>

#include <inttypes.h>


ucs_config_field_t uct_obmm_md_config_table[] = {
    {"", "", NULL, ucs_offsetof(uct_obmm_md_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_md_config_table)},

    {NULL}
};

static ucs_status_t uct_obmm_md_query(uct_md_h md, uct_md_attr_v2_t *attr)
{
    (void)md;
    uct_md_base_md_query(attr);
    attr->flags                  = UCT_MD_FLAG_REG | UCT_MD_FLAG_NEED_RKEY;
    attr->reg_mem_types          = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->reg_nonblock_mem_types = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->cache_mem_types        = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->access_mem_types       = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    return UCS_OK;
}

static void uct_obmm_md_unmap_all(uct_obmm_md_t *md)
{
    unsigned i;

    for (i = 0; i < md->num_regions; ++i) {
        uct_obmm_region_close(&md->regions[i]);
    }
    ucs_free(md->regions);
    md->regions     = NULL;
    md->num_regions = 0;
    md->export_idx  = -1;
}

static void uct_obmm_md_close(uct_md_h tl_md)
{
    uct_obmm_md_t *md = ucs_derived_of(tl_md, uct_obmm_md_t);

    uct_obmm_md_unmap_all(md);
    ucs_free(md);
}

static ucs_status_t uct_obmm_md_map_devices(uct_obmm_md_t *md,
                                            const uct_obmm_dev_info_t *devs,
                                            unsigned num_devs)
{
    uct_obmm_region_t *regions;
    ucs_status_t       status;
    unsigned           i, mapped;
    int                export_idx = -1;

    regions = ucs_calloc(num_devs, sizeof(*regions), "uct_obmm_regions");
    if (regions == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    for (i = 0; i < num_devs; ++i) {
        regions[i].fd = -1;
    }

    mapped = 0;
    for (i = 0; i < num_devs; ++i) {
        status = uct_obmm_region_open(&devs[i], &regions[mapped]);
        if (status != UCS_OK) {
            ucs_warn("obmm: skipping device %s: %s",
                     devs[i].dev_path, ucs_status_string(status));
            continue;
        }

        if ((regions[mapped].info.type == UCT_OBMM_DEV_EXPORT) &&
            (export_idx < 0)) {
            export_idx = (int)mapped;
        } else if (regions[mapped].info.type == UCT_OBMM_DEV_EXPORT) {
            ucs_warn("obmm: multiple export regions found; using first "
                     "(memid=%" PRIu64 "), ignoring memid=%" PRIu64,
                     regions[export_idx].info.memid,
                     regions[mapped].info.memid);
        }

        ++mapped;
    }

    if (mapped == 0) {
        ucs_free(regions);
        return UCS_ERR_NO_DEVICE;
    }

    md->regions     = regions;
    md->num_regions = mapped;
    md->export_idx  = export_idx;
    return UCS_OK;
}

ucs_status_t uct_obmm_md_open(uct_component_t *component, const char *md_name,
                              const uct_md_config_t *config, uct_md_h *md_p)
{
    static uct_md_ops_t md_ops = {
        .close              = uct_obmm_md_close,
        .query              = uct_obmm_md_query,
        .mkey_pack          = ucs_empty_function_return_success,
        .mem_reg            = uct_md_dummy_mem_reg,
        .mem_dereg          = uct_md_dummy_mem_dereg,
        .mem_attach         = ucs_empty_function_return_unsupported,
        .detect_memory_type = ucs_empty_function_return_unsupported
    };
    uct_obmm_dev_info_t *devs    = NULL;
    unsigned             num_devs = 0;
    uct_obmm_md_t       *md;
    ucs_status_t         status;

    (void)component;
    (void)md_name;
    (void)config;

    md = ucs_calloc(1, sizeof(*md), "uct_obmm_md");
    if (md == NULL) {
        ucs_error("failed to allocate obmm md");
        return UCS_ERR_NO_MEMORY;
    }
    md->export_idx = -1;

    status = uct_obmm_sysfs_discover(&devs, &num_devs);
    if (status != UCS_OK) {
        ucs_debug("obmm: sysfs discovery failed: %s",
                  ucs_status_string(status));
        goto err_free_md;
    }

    if (num_devs == 0) {
        ucs_debug("obmm: no shmdev devices found under " UCT_OBMM_SYSFS_ROOT);
        status = UCS_ERR_NO_DEVICE;
        goto err_free_devs;
    }

    status = uct_obmm_md_map_devices(md, devs, num_devs);
    if (status != UCS_OK) {
        ucs_debug("obmm: failed to map any device: %s",
                  ucs_status_string(status));
        goto err_free_devs;
    }

    uct_obmm_sysfs_release(devs);

    md->super.ops       = &md_ops;
    md->super.component = &uct_obmm_component;
    *md_p               = &md->super;
    return UCS_OK;

err_free_devs:
    uct_obmm_sysfs_release(devs);
err_free_md:
    ucs_free(md);
    return status;
}

ucs_status_t uct_obmm_md_rkey_unpack(uct_component_t *component,
                                     const void *rkey_buffer,
                                     uct_rkey_t *rkey_p, void **handle_p)
{
    (void)component;
    (void)rkey_buffer;
    *rkey_p   = 0;
    *handle_p = NULL;
    return UCS_OK;
}

uct_component_t uct_obmm_component = {
    .query_md_resources = uct_md_query_single_md_resource,
    .md_open            = uct_obmm_md_open,
    .cm_open            = ucs_empty_function_return_unsupported,
    .rkey_unpack        = uct_obmm_md_rkey_unpack,
    .rkey_ptr           = uct_sm_rkey_ptr,
    .rkey_release       = ucs_empty_function_return_success,
    .rkey_compare       = uct_base_rkey_compare,
    .name               = "obmm",
    .md_config          = {
        .name           = "OBMM memory domain",
        .prefix         = "OBMM_",
        .table          = uct_obmm_md_config_table,
        .size           = sizeof(uct_obmm_md_config_t),
    },
    .cm_config          = UCS_CONFIG_EMPTY_GLOBAL_LIST_ENTRY,
    .tl_list            = UCT_COMPONENT_TL_LIST_INITIALIZER(&uct_obmm_component),
    .flags              = UCT_COMPONENT_FLAG_RKEY_PTR,
    .md_vfs_init        = (uct_component_md_vfs_init_func_t)ucs_empty_function
};
