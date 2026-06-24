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

#include <ucs/debug/assert.h>
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
     "Optional comma-separated list of NC shmdev memids.",
     ucs_offsetof(uct_obmm_md_config_t, nc_memids), UCS_CONFIG_TYPE_STRING},

    {"CC_MEMIDS", "",
     "Optional comma-separated list of same-node cacheable CC shmdev memids. "
     "CC is used only for same-node OBMM AM traffic and never for cross-node "
     "ownership-based zcopy.",
     ucs_offsetof(uct_obmm_md_config_t, cc_memids), UCS_CONFIG_TYPE_STRING},

    {NULL}
};

static const char *uct_obmm_plane_name(uct_obmm_plane_t plane)
{
    return (plane == UCT_OBMM_PLANE_CC) ? "cc" : "nc";
}

static ucs_status_t uct_obmm_md_query(uct_md_h md, uct_md_attr_v2_t *attr)
{
    (void)md;
    uct_md_base_md_query(attr);
    /* obmm is AM-only: NC short/bcopy. It does NOT expose remote memory as a
     * directly-dereferenceable pointer to its peers: only the pre-exported NC
     * transport regions are mmap'd, never the user's send/recv buffers. So we
     * MUST NOT advertise
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

static ucs_status_t
uct_obmm_md_parse_memids(const char *memids_str, uint64_t **memids_p,
                         unsigned *num_memids_p)
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
            ucs_error("obmm: memid list contains an empty entry: '%s'",
                      memids_str);
            status = UCS_ERR_INVALID_PARAM;
            goto err;
        }

        memid = strtoull(cursor, &end, 10);
        if ((errno == ERANGE) || (*end != '\0')) {
            ucs_error("obmm: invalid memid list entry '%s' in '%s'; "
                      "expected decimal memid list like '1,2'",
                      cursor, memids_str);
            status = UCS_ERR_INVALID_PARAM;
            goto err;
        }

        if (memid == 0) {
            ucs_error("obmm: memid list cannot contain memid 0");
            status = UCS_ERR_INVALID_PARAM;
            goto err;
        }

        for (i = 0; i < count; ++i) {
            if (memids[i] == memid) {
                ucs_error("obmm: duplicate memid %" PRIu64
                          " in memid list '%s'",
                          memid, memids_str);
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
    md->regions                       = NULL;
    md->num_regions                   = 0;
    md->export_idx[UCT_OBMM_PLANE_NC] = -1;
    md->export_idx[UCT_OBMM_PLANE_CC] = -1;
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
    unsigned           i, mapped, j;
    int                export_idx[UCT_OBMM_PLANE_LAST] = { -1, -1 };

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
            ucs_debug("obmm: mapping %s (memid=%" PRIu64 ") failed: %s",
                      devs[i].dev_path, devs[i].memid,
                      ucs_status_string(status));
            goto err_unmap;
        }

        if (regions[mapped].info.type == UCT_OBMM_DEV_EXPORT) {
            uct_obmm_plane_t plane = regions[mapped].info.plane;

            if (export_idx[plane] < 0) {
                export_idx[plane] = (int)mapped;
            } else {
                ucs_debug("obmm: multiple %s export regions found "
                          "(memid=%" PRIu64 ", memid=%" PRIu64 ")",
                          uct_obmm_plane_name(plane),
                          regions[export_idx[plane]].info.memid,
                          regions[mapped].info.memid);
                uct_obmm_region_close(&regions[mapped]);
                status = UCS_ERR_INVALID_PARAM;
                goto err_unmap;
            }
        }

        ++mapped;
    }

    if (mapped == 0) {
        ucs_free(regions);
        return UCS_ERR_NO_DEVICE;
    }

    if ((export_idx[UCT_OBMM_PLANE_NC] < 0) &&
        (export_idx[UCT_OBMM_PLANE_CC] < 0)) {
        ucs_debug("obmm: no local OBMM export region found");
        status = UCS_ERR_NO_DEVICE;
        goto err_unmap;
    }

    md->regions                       = regions;
    md->num_regions                   = mapped;
    md->export_idx[UCT_OBMM_PLANE_NC] = export_idx[UCT_OBMM_PLANE_NC];
    md->export_idx[UCT_OBMM_PLANE_CC] = export_idx[UCT_OBMM_PLANE_CC];
    return UCS_OK;

err_unmap:
    for (j = 0; j < mapped; ++j) {
        uct_obmm_region_close(&regions[j]);
    }
    ucs_free(regions);
    return status;
}

static int uct_obmm_md_memid_in_list(uint64_t memid, const uint64_t *memids,
                                     unsigned num_memids)
{
    unsigned i;

    for (i = 0; i < num_memids; ++i) {
        if (memids[i] == memid) {
            return 1;
        }
    }
    return 0;
}

static ucs_status_t
uct_obmm_md_discover_explicit_planes(uct_obmm_dev_info_t **devs_p,
                                     const uint64_t *nc_memids,
                                     unsigned num_nc_memids,
                                     const uint64_t *cc_memids,
                                     unsigned num_cc_memids)
{
    uct_obmm_dev_info_t *devs = NULL;
    uint64_t            *all_memids;
    unsigned             total_memids = num_nc_memids + num_cc_memids;
    unsigned             i, n;
    ucs_status_t         status;

    all_memids = ucs_calloc(total_memids, sizeof(*all_memids),
                            "uct_obmm_explicit_memids");
    if (all_memids == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    n = 0;
    for (i = 0; i < num_nc_memids; ++i) {
        if (uct_obmm_md_memid_in_list(nc_memids[i], all_memids, n)) {
            ucs_error("obmm: duplicate NC memid %" PRIu64, nc_memids[i]);
            status = UCS_ERR_INVALID_PARAM;
            goto out_free_memids;
        }
        all_memids[n++] = nc_memids[i];
    }

    for (i = 0; i < num_cc_memids; ++i) {
        if (uct_obmm_md_memid_in_list(cc_memids[i], all_memids, n)) {
            ucs_error("obmm: memid %" PRIu64 " appears in both "
                      "OBMM_NC_MEMIDS and OBMM_CC_MEMIDS", cc_memids[i]);
            status = UCS_ERR_INVALID_PARAM;
            goto out_free_memids;
        }
        all_memids[n++] = cc_memids[i];
    }

    status = uct_obmm_sysfs_discover(&devs, all_memids, total_memids);
    if (status != UCS_OK) {
        goto out_free_memids;
    }

    for (i = 0; i < total_memids; ++i) {
        if (uct_obmm_md_memid_in_list(devs[i].memid, nc_memids,
                                      num_nc_memids)) {
            devs[i].plane = UCT_OBMM_PLANE_NC;
        } else {
            ucs_assert(uct_obmm_md_memid_in_list(devs[i].memid, cc_memids,
                                                 num_cc_memids));
            devs[i].plane = UCT_OBMM_PLANE_CC;
        }
    }

    *devs_p = devs;
    status  = UCS_OK;

out_free_memids:
    ucs_free(all_memids);
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
    uint64_t                   *nc_memids     = NULL;
    uint64_t                   *cc_memids     = NULL;
    unsigned                    num_devs      = 0;
    unsigned                    num_nc_memids     = 0;
    unsigned                    num_cc_memids     = 0;
    uct_obmm_md_t              *md;
    ucs_status_t                status;

    (void)component;
    (void)md_name;

    md = ucs_calloc(1, sizeof(*md), "uct_obmm_md");
    if (md == NULL) {
        ucs_error("failed to allocate obmm md");
        return UCS_ERR_NO_MEMORY;
    }
    md->export_idx[UCT_OBMM_PLANE_NC] = -1;
    md->export_idx[UCT_OBMM_PLANE_CC] = -1;

    status = uct_obmm_md_parse_memids(md_config->nc_memids, &nc_memids,
                                      &num_nc_memids);
    if (status != UCS_OK) {
        goto err_free_md;
    }

    status = uct_obmm_md_parse_memids(md_config->cc_memids, &cc_memids,
                                      &num_cc_memids);
    if (status != UCS_OK) {
        goto err_free_nc;
    }

    if ((num_nc_memids == 0) && (num_cc_memids == 0)) {
        ucs_debug("obmm: no OBMM_NC_MEMIDS or OBMM_CC_MEMIDS configured");
        status = UCS_ERR_NO_DEVICE;
        goto err_free_cc;
    }

    num_devs = num_nc_memids + num_cc_memids;
    status = uct_obmm_md_discover_explicit_planes(&devs, nc_memids,
                                                  num_nc_memids, cc_memids,
                                                  num_cc_memids);
    if (status != UCS_OK) {
        goto err_free_cc;
    }

    status = uct_obmm_md_map_devices(md, devs, num_devs);
    if (status != UCS_OK) {
        ucs_debug("obmm: failed to map any device: %s",
                  ucs_status_string(status));
        goto err_free_discovery;
    }

    uct_obmm_sysfs_release(devs);
    ucs_free(cc_memids);
    ucs_free(nc_memids);

    md->super.ops       = &md_ops;
    md->super.component = &uct_obmm_component;
    *md_p               = &md->super;
    return UCS_OK;

err_free_discovery:
    uct_obmm_sysfs_release(devs);
err_free_cc:
    ucs_free(cc_memids);
err_free_nc:
    ucs_free(nc_memids);
err_free_md:
    ucs_free(md);
    return status;
}

static int uct_obmm_region_matches(const uct_obmm_region_t *r,
                                   uint64_t exporter_dcna,
                                   const uct_obmm_eid_t *exporter_deid)
{
    return (r->info.exporter_dcna == exporter_dcna) &&
           (r->info.exporter_deid.hi == exporter_deid->hi) &&
           (r->info.exporter_deid.lo == exporter_deid->lo);
}

uct_obmm_region_t *
uct_obmm_md_find_region(uct_obmm_md_t *md, uct_obmm_plane_t plane,
                        uint64_t exporter_dcna,
                        const uct_obmm_eid_t *exporter_deid)
{
    unsigned i;

    for (i = 0; i < md->num_regions; ++i) {
        uct_obmm_region_t *r = &md->regions[i];

        if (r->info.plane != plane) {
            continue;
        }
        if (uct_obmm_region_matches(r, exporter_dcna, exporter_deid)) {
            return r;
        }
    }
    return NULL;
}

uct_obmm_region_t *
uct_obmm_md_find_import_region(uct_obmm_md_t *md, uct_obmm_plane_t plane,
                               uint64_t exporter_dcna,
                               const uct_obmm_eid_t *exporter_deid)
{
    unsigned i;

    for (i = 0; i < md->num_regions; ++i) {
        uct_obmm_region_t *r = &md->regions[i];

        if (r->info.type != UCT_OBMM_DEV_IMPORT) {
            continue;
        }
        if (r->info.plane != plane) {
            continue;
        }
        if (uct_obmm_region_matches(r, exporter_dcna, exporter_deid)) {
            return r;
        }
    }
    return NULL;
}

uct_obmm_region_t *uct_obmm_md_export_region(uct_obmm_md_t *md,
                                             uct_obmm_plane_t plane)
{
    if ((plane >= UCT_OBMM_PLANE_LAST) || (md->export_idx[plane] < 0)) {
        return NULL;
    }
    return &md->regions[md->export_idx[plane]];
}

int uct_obmm_md_has_plane(uct_obmm_md_t *md, uct_obmm_plane_t plane)
{
    return uct_obmm_md_export_region(md, plane) != NULL;
}

int uct_obmm_md_allow_nc_local_loopback(uct_obmm_md_t *md)
{
    return (uct_obmm_md_export_region(md, UCT_OBMM_PLANE_NC) != NULL) &&
           (uct_obmm_md_export_region(md, UCT_OBMM_PLANE_CC) == NULL);
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
