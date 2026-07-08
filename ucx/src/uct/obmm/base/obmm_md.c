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
    /* obmm is AM-only: FIFO short/bcopy over transport regions. It does
     * NOT expose remote memory as a directly-dereferenceable pointer to its
     * peers: only transport FIFO regions are mmap'd, never the user's
     * send/recv buffers. So we MUST NOT advertise
     * UCT_MD_FLAG_NEED_RKEY (which implies remote-key-based access) -- doing
     * so makes UCP pick rendezvous-via-rkey_ptr for messages above the rndv
     * threshold and try to memcpy from a peer-VA pointer, segfaulting inside
     * ucs_memcpy_relaxed. */
    attr->flags                  = 0;
    attr->reg_mem_types          = 0;
    attr->reg_nonblock_mem_types = 0;
    attr->cache_mem_types        = 0;
    attr->access_mem_types       = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    return UCS_OK;
}

static void uct_obmm_md_close(uct_md_h tl_md)
{
    uct_obmm_md_t *md = ucs_derived_of(tl_md, uct_obmm_md_t);

    ucs_free(md);
}

ucs_status_t
uct_obmm_md_discover_devices(uct_obmm_md_t *md,
                             uct_obmm_dev_info_t **devices_p,
                             unsigned *num_devices_p)
{
    ucs_status_t status;

    status = uct_obmm_sysfs_discover(devices_p, num_devices_p);
    if (status != UCS_OK) {
        return status;
    }

    uct_obmm_sysfs_set_exporter_cna(*devices_p, *num_devices_p,
                                    md->local_cna);
    return UCS_OK;
}

static int
uct_obmm_dev_identity_matches(const uct_obmm_dev_info_t *dev,
                              uint64_t exporter_dcna,
                              const uct_obmm_eid_t *exporter_deid,
                              uint32_t region_id)
{
    return (dev->exporter_dcna == exporter_dcna) &&
           (dev->exporter_deid.hi == exporter_deid->hi) &&
           (dev->exporter_deid.lo == exporter_deid->lo) &&
           (dev->region_id == region_id);
}

ucs_status_t
uct_obmm_md_find_device(uct_obmm_md_t *md, uint64_t exporter_dcna,
                        const uct_obmm_eid_t *exporter_deid,
                        uint32_t region_id, uct_obmm_dev_info_t *info_p)
{
    uct_obmm_dev_info_t *devices = NULL;
    uct_obmm_dev_info_t *match   = NULL;
    ucs_status_t         status;
    unsigned             i, num_devices = 0;

    status = uct_obmm_md_discover_devices(md, &devices, &num_devices);
    if (status != UCS_OK) {
        return status;
    }

    for (i = 0; i < num_devices; ++i) {
        if (!uct_obmm_dev_identity_matches(&devices[i], exporter_dcna,
                                           exporter_deid, region_id)) {
            continue;
        }

        if (match != NULL) {
            ucs_error("obmm: ambiguous shmdev identity for memids %" PRIu64
                      " and %" PRIu64 " (dcna=0x%" PRIx64
                      " deid=0x%" PRIx64 ":0x%" PRIx64
                      " region_id=0x%x)",
                      match->memid, devices[i].memid, exporter_dcna,
                      exporter_deid->hi, exporter_deid->lo, region_id);
            status = UCS_ERR_INVALID_PARAM;
            goto out;
        }

        match = &devices[i];
    }

    if (match == NULL) {
        status = UCS_ERR_UNREACHABLE;
        goto out;
    }

    *info_p = *match;
    status  = UCS_OK;

out:
    uct_obmm_sysfs_release(devices);
    return status;
}

ucs_status_t uct_obmm_md_open(uct_component_t *component, const char *md_name,
                              const uct_md_config_t *config, uct_md_h *md_p)
{
    static uct_md_ops_t md_ops = {
        .close              = uct_obmm_md_close,
        .query              = uct_obmm_md_query,
        .mem_alloc          = (uct_md_mem_alloc_func_t)
                              ucs_empty_function_return_unsupported,
        .mem_free           = (uct_md_mem_free_func_t)
                              ucs_empty_function_return_unsupported,
        .mem_advise         = (uct_md_mem_advise_func_t)
                              ucs_empty_function_return_unsupported,
        .mkey_pack          = (uct_md_mkey_pack_func_t)
                              ucs_empty_function_return_unsupported,
        .mem_reg            = (uct_md_mem_reg_func_t)
                              ucs_empty_function_return_unsupported,
        .mem_dereg          = (uct_md_mem_dereg_func_t)
                              ucs_empty_function_return_unsupported,
        .mem_query          = (uct_md_mem_query_func_t)
                              ucs_empty_function_return_unsupported,
        .mem_attach         = (uct_md_mem_attach_func_t)
                              ucs_empty_function_return_unsupported,
        .detect_memory_type = (uct_md_detect_memory_type_func_t)
                              ucs_empty_function_return_unsupported
    };
    uct_obmm_md_t              *md;
    ucs_status_t                status;

    (void)component;
    (void)md_name;
    (void)config;

    md = ucs_calloc(1, sizeof(*md), "uct_obmm_md");
    if (md == NULL) {
        ucs_error("failed to allocate obmm md");
        return UCS_ERR_NO_MEMORY;
    }

    status = uct_obmm_sysfs_read_local_identity(&md->local_cna,
                                                &md->local_eid);
    if (status != UCS_OK) {
        ucs_error("obmm: failed to read local CNA/EID from sysfs: %s",
                  ucs_status_string(status));
        goto err_free_md;
    }

    ucs_debug("obmm: local identity cna=0x%" PRIx64
              " eid=0x%" PRIx64 ":0x%" PRIx64,
              md->local_cna, md->local_eid.hi, md->local_eid.lo);

    md->super.ops       = &md_ops;
    md->super.component = &uct_obmm_component;
    *md_p               = &md->super;
    return UCS_OK;

err_free_md:
    ucs_free(md);
    return status;
}

uct_component_t uct_obmm_component = {
    .query_md_resources = uct_md_query_single_md_resource,
    .md_open            = uct_obmm_md_open,
    .cm_open            = (uct_component_cm_open_func_t)
                          ucs_empty_function_return_unsupported,
    .rkey_unpack        = (uct_component_rkey_unpack_func_t)
                          ucs_empty_function_return_unsupported,
    /* No rkey_ptr: obmm cannot expose remote process buffers via a local
     * pointer (only the transport FIFO region is mmap'd, never the peer's
     * user heap). Advertising rkey_ptr would make UCP rendezvous pick
     * rndv-via-rkey_ptr above the rndv threshold and segfault on memcpy from a
     * peer-VA pointer. */
    .rkey_ptr           = (uct_component_rkey_ptr_func_t)
                          ucs_empty_function_return_unsupported,
    .rkey_release       = (uct_component_rkey_release_func_t)
                          ucs_empty_function_return_unsupported,
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
    .flags              = 0,
    .md_vfs_init        = (uct_component_md_vfs_init_func_t)ucs_empty_function
};
