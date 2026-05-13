/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "obmm_md.h"

#include <ucs/debug/log.h>
#include <ucs/debug/memtrack_int.h>


static ucs_status_t uct_obmm_rkey_ptr(uct_component_t *component, uct_rkey_t rkey,
                                      void *handle, uint64_t raddr,
                                      void **laddr_p)
{
    /* rkey stores offset from the remote va */
    *laddr_p = UCS_PTR_BYTE_OFFSET(raddr, (ptrdiff_t)rkey);
    return UCS_OK;
}


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

static void uct_obmm_md_close(uct_md_h md)
{
    ucs_free(md);
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
    uct_obmm_md_t *md;

    (void)component;
    (void)md_name;
    (void)config;

    md = ucs_calloc(1, sizeof(*md), "uct_obmm_md");
    if (md == NULL) {
        ucs_error("failed to allocate obmm md");
        return UCS_ERR_NO_MEMORY;
    }

    md->super.ops       = &md_ops;
    md->super.component = &uct_obmm_component;
    *md_p               = &md->super;
    return UCS_OK;
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
    .rkey_ptr           = uct_obmm_rkey_ptr,
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
