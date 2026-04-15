/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "obmm_md.h"
#include "obmm_device.h"
#include "obmm_resource.h"

#include <ucs/debug/log.h>
#include <ucs/sys/string.h>
#include <ucs/type/init_once.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>


static uct_obmm_md_t *uct_obmm_global_md;
static ucs_spinlock_t uct_obmm_global_md_lock;
static ucs_init_once_t uct_obmm_global_md_init_once = UCS_INIT_ONCE_INITIALIZER;


static void uct_obmm_global_md_init()
{
    if (ucs_spinlock_init(&uct_obmm_global_md_lock, 0) != UCS_OK) {
        ucs_fatal("failed to initialize obmm global md lock");
    }
}

static void uct_obmm_global_md_add(uct_obmm_md_t *md)
{
    UCS_INIT_ONCE(&uct_obmm_global_md_init_once) {
        uct_obmm_global_md_init();
    }

    ucs_spin_lock(&uct_obmm_global_md_lock);
    uct_obmm_global_md = md;
    ucs_spin_unlock(&uct_obmm_global_md_lock);
}

static void uct_obmm_global_md_remove(uct_obmm_md_t *md)
{
    UCS_INIT_ONCE(&uct_obmm_global_md_init_once) {
        uct_obmm_global_md_init();
    }

    ucs_spin_lock(&uct_obmm_global_md_lock);
    if (uct_obmm_global_md == md) {
        uct_obmm_global_md = NULL;
    }
    ucs_spin_unlock(&uct_obmm_global_md_lock);
}

ucs_status_t uct_obmm_md_get_device_by_index(uint32_t device_index,
                                             uct_obmm_device_t **device_p)
{
    uct_obmm_md_t *md;
    uct_obmm_device_t *device;

    if (device_p == NULL) {
        return UCS_ERR_INVALID_PARAM;
    }

    UCS_INIT_ONCE(&uct_obmm_global_md_init_once) {
        uct_obmm_global_md_init();
    }

    ucs_spin_lock(&uct_obmm_global_md_lock);
    md = uct_obmm_global_md;
    if ((md == NULL) || (device_index >= md->topology.num_devices)) {
        ucs_spin_unlock(&uct_obmm_global_md_lock);
        return UCS_ERR_NO_DEVICE;
    }

    device = &md->topology.devices[device_index];
    if ((device->flags & UCT_OBMM_DEVICE_FLAG_DEGRADED) ||
        (device->primary_ctl == NULL) || (device->res_domain == NULL)) {
        ucs_spin_unlock(&uct_obmm_global_md_lock);
        return UCS_ERR_NO_DEVICE;
    }

    *device_p = device;
    ucs_spin_unlock(&uct_obmm_global_md_lock);
    return UCS_OK;
}

ucs_config_field_t uct_obmm_md_config_table[] = {
    {"", "", NULL, ucs_offsetof(uct_obmm_md_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_md_config_table)},

    {"CTL_GLOB", UCT_OBMM_DEFAULT_CTL_GLOB,
     "Glob pattern for discovering UB controllers",
     ucs_offsetof(uct_obmm_md_config_t, ctl_glob), UCS_CONFIG_TYPE_STRING},

    {"GRANULARITY", "2m", "OBMM granularity fallback value",
     ucs_offsetof(uct_obmm_md_config_t, granularity), UCS_CONFIG_TYPE_MEMUNITS},

    {"MAX_EXPORT_BYTES", "inf", "Per-socket export quota",
     ucs_offsetof(uct_obmm_md_config_t, max_export_bytes),
     UCS_CONFIG_TYPE_MEMUNITS},

    {"MAX_IMPORT_BYTES", "inf", "Per-socket import quota",
     ucs_offsetof(uct_obmm_md_config_t, max_import_bytes),
     UCS_CONFIG_TYPE_MEMUNITS},

    {"MAX_PREIMPORT_BYTES", "inf", "Per-socket preimport quota",
     ucs_offsetof(uct_obmm_md_config_t, max_preimport_bytes),
     UCS_CONFIG_TYPE_MEMUNITS},

    {"MAX_EXPORTS", "4096", "Per-socket number of export handles",
     ucs_offsetof(uct_obmm_md_config_t, max_exports), UCS_CONFIG_TYPE_UINT},

    {"MAX_IMPORTS", "4096", "Per-socket number of import handles",
     ucs_offsetof(uct_obmm_md_config_t, max_imports), UCS_CONFIG_TYPE_UINT},

    {"MAX_PREIMPORTS", "4096", "Per-socket number of preimport handles",
     ucs_offsetof(uct_obmm_md_config_t, max_preimports), UCS_CONFIG_TYPE_UINT},

    {"DEFAULT_SOCKET", "auto", "Fallback socket id when auto-selection fails",
     ucs_offsetof(uct_obmm_md_config_t, default_socket), UCS_CONFIG_TYPE_STRING},

    {"ENABLE_PREIMPORT", "y", "Enable preimport caching support",
     ucs_offsetof(uct_obmm_md_config_t, enable_preimport), UCS_CONFIG_TYPE_BOOL},

    {NULL}
};

static ucs_status_t uct_obmm_md_query(uct_md_h md, uct_md_attr_v2_t *attr)
{
    uct_obmm_md_t *obmm_md = ucs_derived_of(md, uct_obmm_md_t);

    uct_md_base_md_query(attr);
    attr->flags                  = UCT_MD_FLAG_REG | UCT_MD_FLAG_NEED_RKEY;
    attr->reg_mem_types          = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->reg_nonblock_mem_types = 0;
    attr->cache_mem_types        = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->access_mem_types       = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->alloc_mem_types        = 0;
    attr->rkey_packed_size       = sizeof(uct_obmm_packed_rkey_v1_t) +
                                   UCT_OBMM_MAX_PRIV_LEN;
    attr->local_cpus             = obmm_md->union_local_cpus;
    attr->reg_alignment          = obmm_md->topology.obmm_granularity;
    return UCS_OK;
}

static ucs_status_t
uct_obmm_md_mem_query(uct_md_h md, const void *address, size_t length,
                      uct_md_mem_attr_t *mem_attr)
{
    (void)md;
    (void)address;
    (void)length;
    (void)mem_attr;
    return UCS_ERR_UNSUPPORTED;
}

static void uct_obmm_md_close(uct_md_h md)
{
    uct_obmm_md_t *obmm_md = ucs_derived_of(md, uct_obmm_md_t);

    uct_obmm_global_md_remove(obmm_md);
    uct_obmm_topology_cleanup(&obmm_md->topology);
    ucs_spinlock_destroy(&obmm_md->lock);
    ucs_free(obmm_md);
}

static uct_md_ops_t uct_obmm_md_ops = {
    .close              = uct_obmm_md_close,
    .query              = uct_obmm_md_query,
    .mem_alloc          = (uct_md_mem_alloc_func_t)ucs_empty_function_return_unsupported,
    .mem_free           = (uct_md_mem_free_func_t)ucs_empty_function_return_unsupported,
    .mem_advise         = (uct_md_mem_advise_func_t)ucs_empty_function_return_unsupported,
    .mem_reg            = uct_obmm_md_mem_reg,
    .mem_dereg          = uct_obmm_md_mem_dereg,
    .mem_query          = uct_obmm_md_mem_query,
    .mkey_pack          = uct_obmm_md_mkey_pack,
    .mem_attach         = (uct_md_mem_attach_func_t)ucs_empty_function_return_unsupported,
    .detect_memory_type = (uct_md_detect_memory_type_func_t)ucs_empty_function_return_unsupported
};

static int uct_obmm_parse_default_socket(const char *value, int *socket_id_p)
{
    char *endp;
    long socket_id;

    if ((value == NULL) || !strcmp(value, "auto")) {
        *socket_id_p = UCT_OBMM_DEFAULT_SOCKET_AUTO;
        return 0;
    }

    errno     = 0;
    socket_id = strtol(value, &endp, 10);
    if ((endp == value) || (*endp != '\0') || (errno != 0) || (socket_id < 0)) {
        return -EINVAL;
    }

    *socket_id_p = (int)socket_id;
    return 0;
}

static ucs_status_t
uct_obmm_md_init_runtime_config(uct_obmm_md_t *md,
                                const uct_obmm_md_config_t *config)
{
    int default_socket;

    if (uct_obmm_parse_default_socket(config->default_socket,
                                      &default_socket) != 0) {
        return UCS_ERR_INVALID_PARAM;
    }

    ucs_strncpy_safe(md->config.ctl_glob,
                     (config->ctl_glob != NULL) ? config->ctl_glob :
                                                  UCT_OBMM_DEFAULT_CTL_GLOB,
                     sizeof(md->config.ctl_glob));
    md->config.granularity       = config->granularity;
    md->config.max_export_bytes  = config->max_export_bytes;
    md->config.max_import_bytes  = config->max_import_bytes;
    md->config.max_preimport_bytes = config->max_preimport_bytes;
    md->config.max_exports       = config->max_exports;
    md->config.max_imports       = config->max_imports;
    md->config.max_preimports    = config->max_preimports;
    md->config.default_socket    = default_socket;
    md->config.enable_preimport  = !!config->enable_preimport;

    if (md->config.granularity == 0) {
        md->config.granularity = UCT_OBMM_DEFAULT_GRANULARITY;
    }
    if (md->config.max_exports == 0) {
        md->config.max_exports = UCT_OBMM_DEFAULT_MAX_EXPORTS;
    }
    if (md->config.max_imports == 0) {
        md->config.max_imports = UCT_OBMM_DEFAULT_MAX_IMPORTS;
    }
    if (md->config.max_preimports == 0) {
        md->config.max_preimports = UCT_OBMM_DEFAULT_MAX_PREIMPORTS;
    }

    return UCS_OK;
}

static void uct_obmm_md_build_union_local_cpus(uct_obmm_md_t *md)
{
    unsigned i, cpu;
    uct_obmm_device_t *device;

    UCS_CPU_ZERO(&md->union_local_cpus);
    for (i = 0; i < md->topology.num_devices; ++i) {
        device = &md->topology.devices[i];
        if ((device->flags & UCT_OBMM_DEVICE_FLAG_DEGRADED) ||
            (device->primary_ctl == NULL) || (device->res_domain == NULL)) {
            continue;
        }

        for (cpu = 0; cpu < UCS_CPU_SETSIZE; ++cpu) {
            if (ucs_cpu_is_set(cpu, &device->local_cpus)) {
                UCS_CPU_SET(cpu, &md->union_local_cpus);
            }
        }
    }
}

ucs_status_t uct_obmm_md_open(uct_component_t *component, const char *md_name,
                              const uct_md_config_t *config, uct_md_h *md_p)
{
    const uct_obmm_md_config_t *obmm_config =
            (const uct_obmm_md_config_t*)config;
    uct_obmm_md_t *md;
    ucs_status_t status;

    if ((component != &uct_obmm_component) || (md_name == NULL) ||
        (md_p == NULL) || strcmp(md_name, "obmm")) {
        return UCS_ERR_NO_DEVICE;
    }

    md = ucs_calloc(1, sizeof(*md), "uct_obmm_md");
    if (md == NULL) {
        ucs_error("failed to allocate obmm md");
        return UCS_ERR_NO_MEMORY;
    }

    status = ucs_spinlock_init(&md->lock, 0);
    if (status != UCS_OK) {
        ucs_free(md);
        return status;
    }

    status = uct_obmm_md_init_runtime_config(md, obmm_config);
    if (status != UCS_OK) {
        ucs_spinlock_destroy(&md->lock);
        ucs_free(md);
        return status;
    }

    md->generation = (uint32_t)ucs_generate_uuid((uintptr_t)md);
    status         = uct_obmm_topology_discover(md, &md->topology);
    if (status != UCS_OK) {
        ucs_spinlock_destroy(&md->lock);
        ucs_free(md);
        return status;
    }

    uct_obmm_md_build_union_local_cpus(md);
    md->super.ops       = &uct_obmm_md_ops;
    md->super.component = &uct_obmm_component;
    *md_p               = &md->super;
    uct_obmm_global_md_add(md);
    return UCS_OK;
}

uct_component_t uct_obmm_component = {
    .query_md_resources = uct_md_query_single_md_resource,
    .md_open            = uct_obmm_md_open,
    .cm_open            = ucs_empty_function_return_unsupported,
    .rkey_unpack        = uct_obmm_md_rkey_unpack,
    .rkey_ptr           = uct_obmm_rkey_ptr,
    .rkey_release       = uct_obmm_rkey_release,
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
