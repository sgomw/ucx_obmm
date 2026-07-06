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

    {"MEMIDS", "",
     "Optional comma-separated list of shmdev memids. When omitted, OBMM "
     "scans sysfs and uses all shmdevs whose priv is 'ucx-obmm:NN'. When set, "
     "the list must contain one or more local exports plus the imports needed "
     "to reach remote nodes. Each iface claims one local export block.",
     ucs_offsetof(uct_obmm_md_config_t, memids), UCS_CONFIG_TYPE_STRING},

    {NULL}
};

static ucs_status_t uct_obmm_md_query(uct_md_h md, uct_md_attr_v2_t *attr)
{
    (void)md;
    uct_md_base_md_query(attr);
    /* obmm is AM-only: FIFO short/bcopy over pre-exported NC regions. It does
     * NOT expose remote memory as a directly-dereferenceable pointer to its
     * peers: only pre-exported transport regions are mmap'd, never the user's
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
    md->regions     = NULL;
    md->num_regions = 0;
    md->num_exports = 0;
}

static void uct_obmm_md_close(uct_md_h tl_md)
{
    uct_obmm_md_t *md = ucs_derived_of(tl_md, uct_obmm_md_t);

    uct_obmm_md_unmap_all(md);
    ucs_free(md);
}

static int uct_obmm_dev_identity_matches(const uct_obmm_dev_info_t *a,
                                         const uct_obmm_dev_info_t *b)
{
    return (a->exporter_dcna == b->exporter_dcna) &&
           (a->exporter_deid.hi == b->exporter_deid.hi) &&
           (a->exporter_deid.lo == b->exporter_deid.lo) &&
           (a->region_id == b->region_id);
}

static ucs_status_t
uct_obmm_md_validate_region_identities(const uct_obmm_region_t *regions,
                                       unsigned num_regions)
{
    const uct_obmm_dev_info_t *a;
    const uct_obmm_dev_info_t *b;
    unsigned                   i, j;

    for (i = 0; i < num_regions; ++i) {
        a = &regions[i].info;
        for (j = i + 1; j < num_regions; ++j) {
            b = &regions[j].info;
            if (!uct_obmm_dev_identity_matches(a, b)) {
                continue;
            }

            ucs_error("obmm: ambiguous region identity for memids %" PRIu64
                      " and %" PRIu64 " "
                      "(dcna=0x%" PRIx64 " deid=0x%" PRIx64 ":0x%" PRIx64
                      " region_id=0x%x). Configure unique shmdev priv "
                      "metadata for each pre-exported block.",
                      a->memid, b->memid, a->exporter_dcna,
                      a->exporter_deid.hi,
                      a->exporter_deid.lo, a->region_id);
            return UCS_ERR_INVALID_PARAM;
        }
    }

    return UCS_OK;
}

static ucs_status_t uct_obmm_md_map_devices(uct_obmm_md_t *md,
                                            const uct_obmm_dev_info_t *devs,
                                            unsigned num_devs)
{
    uct_obmm_region_t *regions;
    ucs_status_t       status;
    unsigned           i, mapped, j;
    unsigned           num_exports = 0;

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
            ++num_exports;
        }

        ++mapped;
    }

    if (mapped == 0) {
        ucs_free(regions);
        return UCS_ERR_NO_DEVICE;
    }

    status = uct_obmm_md_validate_region_identities(regions, mapped);
    if (status != UCS_OK) {
        goto err_unmap;
    }

    if (num_exports == 0) {
        ucs_error("obmm: no local export found in configured/discovered "
                  "shmdevs");
        status = UCS_ERR_NO_DEVICE;
        goto err_unmap;
    }

    md->regions     = regions;
    md->num_regions = mapped;
    md->num_exports = num_exports;
    return UCS_OK;

err_unmap:
    for (j = 0; j < mapped; ++j) {
        uct_obmm_region_close(&regions[j]);
    }
    ucs_free(regions);
    return status;
}

static ucs_status_t
uct_obmm_md_discover_regions(uct_obmm_dev_info_t **devs_p,
                             unsigned *num_devs_p,
                             const uint64_t *memids,
                             unsigned num_memids)
{
    ucs_status_t         status;

    *devs_p     = NULL;
    *num_devs_p = 0;

    if (num_memids == 0) {
        return uct_obmm_sysfs_discover(devs_p, num_devs_p, NULL, 0);
    }

    status = uct_obmm_sysfs_discover(devs_p, num_devs_p, memids,
                                     num_memids);
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
    const uct_obmm_md_config_t *md_config  = (const uct_obmm_md_config_t*)config;
    uct_obmm_dev_info_t        *devs       = NULL;
    uint64_t                   *memids               = NULL;
    unsigned                    num_devs             = 0;
    unsigned                    num_memids           = 0;
    uct_obmm_md_t              *md;
    ucs_status_t                status;

    (void)component;
    (void)md_name;

    md = ucs_calloc(1, sizeof(*md), "uct_obmm_md");
    if (md == NULL) {
        ucs_error("failed to allocate obmm md");
        return UCS_ERR_NO_MEMORY;
    }

    status = uct_obmm_md_parse_memids(md_config->memids, &memids,
                                      &num_memids);
    if (status != UCS_OK) {
        goto err_free_md;
    }

    status = uct_obmm_md_discover_regions(&devs, &num_devs, memids,
                                          num_memids);
    if (status != UCS_OK) {
        goto err_free_memids;
    }

    status = uct_obmm_md_map_devices(md, devs, num_devs);
    if (status != UCS_OK) {
        ucs_debug("obmm: failed to map any device: %s",
                  ucs_status_string(status));
        goto err_free_discovery;
    }

    uct_obmm_sysfs_release(devs);
    ucs_free(memids);

    md->super.ops       = &md_ops;
    md->super.component = &uct_obmm_component;
    *md_p               = &md->super;
    return UCS_OK;

err_free_discovery:
    uct_obmm_sysfs_release(devs);
err_free_memids:
    ucs_free(memids);
err_free_md:
    ucs_free(md);
    return status;
}

static int uct_obmm_region_matches(const uct_obmm_region_t *r,
                                   uint64_t exporter_dcna,
                                   const uct_obmm_eid_t *exporter_deid,
                                   uint32_t region_id)
{
    return (r->info.exporter_dcna == exporter_dcna) &&
           (r->info.exporter_deid.hi == exporter_deid->hi) &&
           (r->info.exporter_deid.lo == exporter_deid->lo) &&
           (r->info.region_id == region_id);
}

uct_obmm_region_t *
uct_obmm_md_find_region(uct_obmm_md_t *md, uint64_t exporter_dcna,
                        const uct_obmm_eid_t *exporter_deid,
                        uint32_t region_id)
{
    unsigned i;

    for (i = 0; i < md->num_regions; ++i) {
        uct_obmm_region_t *r = &md->regions[i];

        if (uct_obmm_region_matches(r, exporter_dcna, exporter_deid,
                                    region_id)) {
            return r;
        }
    }
    return NULL;
}

uct_obmm_region_t *
uct_obmm_md_find_import_region(uct_obmm_md_t *md, uint64_t exporter_dcna,
                               const uct_obmm_eid_t *exporter_deid,
                               uint32_t region_id)
{
    unsigned i;

    for (i = 0; i < md->num_regions; ++i) {
        uct_obmm_region_t *r = &md->regions[i];

        if (r->info.type != UCT_OBMM_DEV_IMPORT) {
            continue;
        }
        if (uct_obmm_region_matches(r, exporter_dcna, exporter_deid,
                                    region_id)) {
            return r;
        }
    }
    return NULL;
}

unsigned uct_obmm_md_num_export_regions(uct_obmm_md_t *md)
{
    return md->num_exports;
}

uct_obmm_region_t *uct_obmm_md_export_region(uct_obmm_md_t *md,
                                             unsigned index)
{
    unsigned i;

    for (i = 0; i < md->num_regions; ++i) {
        uct_obmm_region_t *r = &md->regions[i];

        if (r->info.type != UCT_OBMM_DEV_EXPORT) {
            continue;
        }

        if (index == 0) {
            return r;
        }
        --index;
    }

    return NULL;
}

uct_component_t uct_obmm_component = {
    .query_md_resources = uct_md_query_single_md_resource,
    .md_open            = uct_obmm_md_open,
    .cm_open            = (uct_component_cm_open_func_t)
                          ucs_empty_function_return_unsupported,
    .rkey_unpack        = (uct_component_rkey_unpack_func_t)
                          ucs_empty_function_return_unsupported,
    /* No rkey_ptr: obmm cannot expose remote process buffers via a local
     * pointer (only the pre-exported FIFO region is mmap'd, never the
     * peer's user heap). Advertising rkey_ptr would make UCP rendezvous
     * pick rndv-via-rkey_ptr above the rndv threshold and segfault on
     * memcpy from a peer-VA pointer. */
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
