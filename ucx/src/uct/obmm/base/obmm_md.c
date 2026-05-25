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
#include <ucs/sys/string.h>

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>


ucs_config_field_t uct_obmm_md_config_table[] = {
    {"", "", NULL, ucs_offsetof(uct_obmm_md_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_md_config_table)},

    {"NC_MEMIDS", "",
     "Required comma-separated allow-list of NC shmdev memids to use for "
     "the active cross-node remote/eager path, for example \"1,2\". obmm "
     "maps these memids with O_SYNC and requires exactly one NC export among "
     "the discovered devices.",
     ucs_offsetof(uct_obmm_md_config_t, nc_memids), UCS_CONFIG_TYPE_STRING},

    {"CC_MEMIDS", "",
     "Optional comma-separated allow-list of CC shmdev memids to use for "
     "future same-node/bulk paths. When set, obmm maps these memids without "
     "O_SYNC.",
     ucs_offsetof(uct_obmm_md_config_t, cc_memids), UCS_CONFIG_TYPE_STRING},

    {NULL}
};

static ucs_status_t uct_obmm_md_query(uct_md_h md, uct_md_attr_v2_t *attr)
{
    (void)md;
    uct_md_base_md_query(attr);
    /* obmm only does AM (short + bcopy). It does NOT expose remote memory
     * as a directly-dereferenceable pointer to its own peers: only the
     * pre-exported FIFO region is mmap'd, NOT the user's send/recv
     * buffers. So we MUST NOT advertise UCT_MD_FLAG_NEED_RKEY (which
     * implies remote-key-based access) -- doing so makes UCP pick
     * rendezvous-via-rkey_ptr for messages above the rndv threshold
     * (~256 KiB) and try to memcpy from a peer-VA pointer, segfaulting
     * inside ucs_memcpy_relaxed. */
    attr->flags                  = 0;
    attr->reg_mem_types          = 0;
    attr->reg_nonblock_mem_types = 0;
    attr->cache_mem_types        = 0;
    attr->access_mem_types       = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    return UCS_OK;
}

static ucs_status_t
uct_obmm_md_parse_memids(const char *var_name, const char *memids_str,
                         uint64_t **memids_p, unsigned *num_memids_p)
{
    uint64_t     *memids      = NULL;
    char         *cursor;
    char         *next;
    char         *copy        = NULL;
    char         *end;
    ucs_status_t  status      = UCS_OK;
    unsigned      count       = 0;
    unsigned      capacity    = 1;
    unsigned      i;
    uint64_t      memid;

    *memids_p     = NULL;
    *num_memids_p = 0;

    if (memids_str == NULL) {
        return UCS_OK;
    }

    copy = ucs_strdup(memids_str, "obmm_memids");
    if (copy == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    ucs_strtrim(copy);
    if (copy[0] == '\0') {
        goto out;
    }

    for (cursor = copy; *cursor != '\0'; ++cursor) {
        if (*cursor == ',') {
            ++capacity;
        }
    }

    memids = ucs_calloc(capacity, sizeof(*memids), "obmm_memid_list");
    if (memids == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto err;
    }

    cursor = copy;
    while (cursor != NULL) {
        errno = 0;
        next  = strchr(cursor, ',');
        if (next != NULL) {
            *next = '\0';
            ++next;
        }

        ucs_strtrim(cursor);
        if (cursor[0] == '\0') {
            ucs_error("obmm: %s contains an empty entry: '%s'",
                      var_name, memids_str);
            status = UCS_ERR_INVALID_PARAM;
            goto err;
        }

        memid = strtoull(cursor, &end, 10);
        if ((errno == ERANGE) || (*end != '\0')) {
            ucs_error("obmm: invalid %s entry '%s' in '%s'; "
                      "expected decimal memid list like '1,2'",
                      var_name, cursor, memids_str);
            status = UCS_ERR_INVALID_PARAM;
            goto err;
        }

        if (memid == 0) {
            ucs_error("obmm: %s cannot contain memid 0", var_name);
            status = UCS_ERR_INVALID_PARAM;
            goto err;
        }

        for (i = 0; i < count; ++i) {
            if (memids[i] == memid) {
                ucs_error("obmm: duplicate memid %" PRIu64 " in %s='%s'",
                          memid, var_name, memids_str);
                status = UCS_ERR_INVALID_PARAM;
                goto err;
            }
        }

        memids[count++] = memid;
        cursor          = next;
    }

out:
    ucs_free(copy);
    *memids_p     = memids;
    *num_memids_p = count;
    return UCS_OK;

err:
    ucs_free(memids);
    ucs_free(copy);
    return status;
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
    md->nc_export_idx = -1;
    md->cc_export_idx = -1;
}

static void uct_obmm_md_close(uct_md_h tl_md)
{
    uct_obmm_md_t *md = ucs_derived_of(tl_md, uct_obmm_md_t);

    uct_obmm_md_unmap_all(md);
    ucs_free(md);
}

static ucs_status_t
uct_obmm_md_append_devices(uct_obmm_md_t *md, const uct_obmm_dev_info_t *devs,
                           unsigned num_devs, uct_obmm_map_mode_t map_mode)
{
    uct_obmm_region_t *regions;
    ucs_status_t       status;
    unsigned           i, mapped, base_count, j;
    int               *export_idx_p;
    int                old_export_idx;

    if (num_devs == 0) {
        return UCS_OK;
    }

    base_count = md->num_regions;
    regions = ucs_realloc(md->regions, (base_count + num_devs) * sizeof(*regions),
                          "uct_obmm_regions");
    if (regions == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    md->regions = regions;
    for (i = base_count; i < (base_count + num_devs); ++i) {
        memset(&regions[i], 0, sizeof(regions[i]));
        regions[i].fd = -1;
    }

    export_idx_p = (map_mode == UCT_OBMM_MAP_MODE_NC) ? &md->nc_export_idx :
                                                      &md->cc_export_idx;
    old_export_idx = *export_idx_p;
    mapped = 0;
    for (i = 0; i < num_devs; ++i) {
        status = uct_obmm_region_open(&devs[i], map_mode,
                                      &regions[base_count + mapped]);
        if (status != UCS_OK) {
            ucs_debug("obmm: mapping %s (memid=%" PRIu64 ") failed: %s",
                      devs[i].dev_path, devs[i].memid,
                      ucs_status_string(status));
            goto err_unmap;
        }

        if ((regions[base_count + mapped].info.type == UCT_OBMM_DEV_EXPORT) &&
            (*export_idx_p < 0)) {
            *export_idx_p = (int)(base_count + mapped);
        } else if (regions[base_count + mapped].info.type == UCT_OBMM_DEV_EXPORT) {
            ucs_debug("obmm: multiple %s export regions found "
                      "(memid=%" PRIu64 ", memid=%" PRIu64 "); obmm "
                      "requires exactly one local export per mapping mode",
                      (map_mode == UCT_OBMM_MAP_MODE_NC) ? "NC" : "CC",
                      regions[*export_idx_p].info.memid,
                      regions[base_count + mapped].info.memid);
            uct_obmm_region_close(&regions[base_count + mapped]);
            status = UCS_ERR_INVALID_PARAM;
            goto err_unmap;
        }

        ++mapped;
    }

    md->num_regions = base_count + mapped;
    return UCS_OK;

err_unmap:
    for (j = 0; j < mapped; ++j) {
        uct_obmm_region_close(&regions[base_count + j]);
    }
    *export_idx_p   = old_export_idx;
    md->num_regions = base_count;
    if (base_count == 0) {
        ucs_free(md->regions);
        md->regions = NULL;
    } else {
        regions = ucs_realloc(md->regions, base_count * sizeof(*regions),
                              "uct_obmm_regions");
        if (regions != NULL) {
            md->regions = regions;
        }
    }
    return status;
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
    const uct_obmm_md_config_t *md_config  = (const uct_obmm_md_config_t*)config;
    uct_obmm_dev_info_t        *devs       = NULL;
    uint64_t                   *nc_memids  = NULL;
    uint64_t                   *cc_memids  = NULL;
    unsigned                    num_devs   = 0;
    unsigned                    num_nc_memids = 0;
    unsigned                    num_cc_memids = 0;
    uct_obmm_md_t              *md;
    ucs_status_t                status;

    (void)component;
    (void)md_name;

    md = ucs_calloc(1, sizeof(*md), "uct_obmm_md");
    if (md == NULL) {
        ucs_error("failed to allocate obmm md");
        return UCS_ERR_NO_MEMORY;
    }
    md->nc_export_idx = -1;
    md->cc_export_idx = -1;

    status = uct_obmm_md_parse_memids("OBMM_NC_MEMIDS", md_config->nc_memids,
                                      &nc_memids,
                                      &num_nc_memids);
    if (status != UCS_OK) {
        goto err_free_md;
    }

    if (num_nc_memids == 0) {
        ucs_error("obmm: OBMM_NC_MEMIDS is required and must list the NC "
                  "export/import memids, for example '1,2'");
        status = UCS_ERR_INVALID_PARAM;
        goto err_free_nc_memids;
    }

    status = uct_obmm_md_parse_memids("OBMM_CC_MEMIDS", md_config->cc_memids,
                                      &cc_memids,
                                      &num_cc_memids);
    if (status != UCS_OK) {
        goto err_free_nc_memids;
    }

    status = uct_obmm_sysfs_discover(&devs, &num_devs, nc_memids,
                                     num_nc_memids);
    if (status != UCS_OK) {
        ucs_debug("obmm: NC sysfs discovery failed: %s",
                  ucs_status_string(status));
        goto err_free_cc_memids;
    }

    if (num_devs == 0) {
        ucs_debug("obmm: no NC shmdev devices found");
        status = UCS_ERR_NO_DEVICE;
        goto err_free_discovery;
    }

    status = uct_obmm_md_append_devices(md, devs, num_devs,
                                        UCT_OBMM_MAP_MODE_NC);
    if (status != UCS_OK) {
        ucs_debug("obmm: failed to map NC devices: %s",
                  ucs_status_string(status));
        goto err_free_discovery;
    }

    uct_obmm_sysfs_release(devs);
    devs     = NULL;
    num_devs = 0;

    if (num_cc_memids > 0) {
        status = uct_obmm_sysfs_discover(&devs, &num_devs, cc_memids,
                                         num_cc_memids);
        if (status != UCS_OK) {
            ucs_debug("obmm: CC sysfs discovery failed: %s",
                      ucs_status_string(status));
            goto err_free_discovery;
        }

        if (num_devs == 0) {
            ucs_debug("obmm: no CC shmdev devices found");
            status = UCS_ERR_NO_DEVICE;
            goto err_free_discovery;
        }

        status = uct_obmm_md_append_devices(md, devs, num_devs,
                                            UCT_OBMM_MAP_MODE_CC);
        if (status != UCS_OK) {
            ucs_debug("obmm: failed to map CC devices: %s",
                      ucs_status_string(status));
            goto err_free_discovery;
        }
    }

    if (md->nc_export_idx < 0) {
        ucs_debug("obmm: no local NC export region found for current obmm transport");
        status = UCS_ERR_NO_DEVICE;
        goto err_free_discovery;
    }

    uct_obmm_sysfs_release(devs);
    ucs_free(nc_memids);
    ucs_free(cc_memids);

    md->super.ops       = &md_ops;
    md->super.component = &uct_obmm_component;
    *md_p               = &md->super;
    return UCS_OK;

err_free_discovery:
    uct_obmm_sysfs_release(devs);
    uct_obmm_md_unmap_all(md);
err_free_cc_memids:
    ucs_free(cc_memids);
err_free_nc_memids:
    ucs_free(nc_memids);
err_free_md:
    ucs_free(md);
    return status;
}

uct_obmm_region_t *
uct_obmm_md_find_import_region_by_mode(uct_obmm_md_t *md,
                                       uct_obmm_map_mode_t map_mode,
                                       uint64_t exporter_dcna,
                                       const uct_obmm_eid_t *exporter_deid)
{
    unsigned i;

    for (i = 0; i < md->num_regions; ++i) {
        uct_obmm_region_t *r = &md->regions[i];

        if ((r->info.type != UCT_OBMM_DEV_IMPORT) ||
            (r->map_mode != map_mode)) {
            continue;
        }
        if ((r->info.exporter_dcna == exporter_dcna) &&
            (r->info.exporter_deid.hi == exporter_deid->hi) &&
            (r->info.exporter_deid.lo == exporter_deid->lo)) {
            return r;
        }
    }
    return NULL;
}

uct_obmm_region_t *
uct_obmm_md_find_import_region_by_mode_memid(uct_obmm_md_t *md,
                                             uct_obmm_map_mode_t map_mode,
                                             uint64_t exporter_dcna,
                                             const uct_obmm_eid_t *exporter_deid,
                                             uint64_t memid)
{
    unsigned i;

    for (i = 0; i < md->num_regions; ++i) {
        uct_obmm_region_t *r = &md->regions[i];

        if ((r->info.type != UCT_OBMM_DEV_IMPORT) ||
            (r->map_mode != map_mode) || (r->info.memid != memid)) {
            continue;
        }
        if ((r->info.exporter_dcna == exporter_dcna) &&
            (r->info.exporter_deid.hi == exporter_deid->hi) &&
            (r->info.exporter_deid.lo == exporter_deid->lo)) {
            return r;
        }
    }
    return NULL;
}

uct_obmm_region_t *
uct_obmm_md_find_import_region(uct_obmm_md_t *md, uint64_t exporter_dcna,
                               const uct_obmm_eid_t *exporter_deid)
{
    return uct_obmm_md_find_import_region_by_mode(md, UCT_OBMM_MAP_MODE_NC,
                                                  exporter_dcna,
                                                  exporter_deid);
}

uct_obmm_region_t *
uct_obmm_md_export_region_by_mode(uct_obmm_md_t *md, uct_obmm_map_mode_t mode)
{
    int export_idx = (mode == UCT_OBMM_MAP_MODE_NC) ? md->nc_export_idx :
                                                      md->cc_export_idx;

    if (export_idx < 0) {
        return NULL;
    }
    return &md->regions[export_idx];
}

uct_obmm_region_t *uct_obmm_md_export_region(uct_obmm_md_t *md)
{
    return uct_obmm_md_export_region_by_mode(md, UCT_OBMM_MAP_MODE_NC);
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
    /* No rkey_ptr: obmm cannot expose remote process buffers via a local
     * pointer (only the pre-exported FIFO region is mmap'd, never the
     * peer's user heap). Advertising rkey_ptr would make UCP rendezvous
     * pick rndv-via-rkey_ptr above the rndv threshold and segfault on
     * memcpy from a peer-VA pointer. */
    .rkey_ptr           = ucs_empty_function_return_unsupported,
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
    .flags              = 0,
    .md_vfs_init        = (uct_component_md_vfs_init_func_t)ucs_empty_function
};
