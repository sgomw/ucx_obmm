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

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>


ucs_config_field_t uct_obmm_md_config_table[] = {
    {"", "", NULL, ucs_offsetof(uct_obmm_md_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_md_config_table)},

    {"MEM_MODE", "nc",
     "OBMM memory mode: nc (v2-compatible NC control/data path) or hybrid "
     "(NC control path + CC am_bcopy payload chunks). Pure cc is not supported "
     "in v3.",
     ucs_offsetof(uct_obmm_md_config_t, mem_mode), UCS_CONFIG_TYPE_STRING},

    {"NC_MEMIDS", "",
     "Comma-separated decimal memids that are non-cacheable OBMM regions. "
     "Mandatory in v3 because NC/CC role is not discoverable from sysfs.",
     ucs_offsetof(uct_obmm_md_config_t, nc_memids), UCS_CONFIG_TYPE_STRING},

    {"CC_MEMIDS", "",
     "Comma-separated decimal memids that are cacheable OBMM regions. "
     "Mandatory when MEM_MODE=hybrid; ignored with warning in MEM_MODE=nc.",
     ucs_offsetof(uct_obmm_md_config_t, cc_memids), UCS_CONFIG_TYPE_STRING},

    {NULL}
};


typedef struct {
    uint64_t *values;
    unsigned  count;
} uct_obmm_memid_list_t;


static ucs_status_t uct_obmm_md_query(uct_md_h md, uct_md_attr_v2_t *attr)
{
    (void)md;
    uct_md_base_md_query(attr);
    attr->flags                  = UCT_MD_FLAG_ALLOC;
    attr->max_alloc              = ULONG_MAX;
    attr->alloc_mem_types        = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->reg_mem_types          = 0;
    attr->reg_nonblock_mem_types = 0;
    attr->cache_mem_types        = 0;
    attr->access_mem_types       = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    return UCS_OK;
}


static ucs_status_t
uct_obmm_md_mem_alloc(uct_md_h md, size_t *length_p, void **address_p,
                      ucs_memory_type_t mem_type, unsigned flags,
                      const char *alloc_name, uct_mem_h *memh_p)
{
    void *address;

    (void)md;
    (void)flags;

    if (mem_type != UCS_MEMORY_TYPE_HOST) {
        return UCS_ERR_UNSUPPORTED;
    }

    if ((*length_p == 0) || (*address_p != NULL)) {
        return UCS_ERR_INVALID_PARAM;
    }

    address = ucs_malloc(*length_p, alloc_name);
    if (address == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    *address_p = address;
    *memh_p    = address;
    return UCS_OK;
}


static ucs_status_t uct_obmm_md_mem_free(uct_md_h md, uct_mem_h memh)
{
    (void)md;

    ucs_free(memh);
    return UCS_OK;
}


static int uct_obmm_memid_list_contains(const uct_obmm_memid_list_t *list,
                                        uint64_t memid)
{
    unsigned i;

    for (i = 0; i < list->count; ++i) {
        if (list->values[i] == memid) {
            return 1;
        }
    }
    return 0;
}


static ucs_status_t
uct_obmm_parse_memid_list(const char *csv, const char *name,
                          uct_obmm_memid_list_t *list)
{
    char       *copy, *token, *saveptr, *end;
    uint64_t   value;
    unsigned   count = 0;

    list->values = NULL;
    list->count  = 0;

    if ((csv == NULL) || (*csv == '\0')) {
        return UCS_OK;
    }

    copy = ucs_strdup(csv, "obmm_memids");
    if (copy == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    for (token = strtok_r(copy, ",", &saveptr); token != NULL;
         token = strtok_r(NULL, ",", &saveptr)) {
        while (isspace((unsigned char)*token)) {
            ++token;
        }
        if (*token == '\0') {
            ucs_error("obmm: empty token in %s='%s'", name, csv);
            ucs_free(copy);
            return UCS_ERR_INVALID_PARAM;
        }

        value = strtoull(token, &end, 10);
        while (isspace((unsigned char)*end)) {
            ++end;
        }
        if ((*end != '\0') || (value == 0)) {
            ucs_error("obmm: invalid memid token '%s' in %s='%s'",
                      token, name, csv);
            ucs_free(copy);
            return UCS_ERR_INVALID_PARAM;
        }

        if (uct_obmm_memid_list_contains(list, value)) {
            ucs_error("obmm: duplicate memid %" PRIu64 " in %s", value, name);
            ucs_free(copy);
            return UCS_ERR_INVALID_PARAM;
        }

        {
            uint64_t *new_values;

            new_values = ucs_realloc(list->values,
                                     (count + 1) * sizeof(uint64_t),
                                     "obmm_memid_list");
            if (new_values == NULL) {
                ucs_free(copy);
                return UCS_ERR_NO_MEMORY;
            }
            list->values = new_values;
        }
        list->values[count++] = value;
        list->count           = count;
    }

    ucs_free(copy);
    return UCS_OK;
}


static void uct_obmm_memid_list_cleanup(uct_obmm_memid_list_t *list)
{
    ucs_free(list->values);
    list->values = NULL;
    list->count  = 0;
}


static ucs_status_t uct_obmm_md_parse_mode(const char *mode_str,
                                           uct_obmm_mem_mode_t *mode_p)
{
    if (!strcmp(mode_str, "nc")) {
        *mode_p = UCT_OBMM_MEM_MODE_NC;
        return UCS_OK;
    }
    if (!strcmp(mode_str, "hybrid")) {
        *mode_p = UCT_OBMM_MEM_MODE_HYBRID;
        return UCS_OK;
    }
    if (!strcmp(mode_str, "cc")) {
        ucs_error("obmm: MEM_MODE=cc is not supported in v3; use nc or hybrid");
        return UCS_ERR_UNSUPPORTED;
    }

    ucs_error("obmm: invalid MEM_MODE='%s' (expected nc or hybrid)", mode_str);
    return UCS_ERR_INVALID_PARAM;
}


static void uct_obmm_md_unmap_all(uct_obmm_md_t *md)
{
    unsigned i;

    for (i = 0; i < md->num_nc_regions; ++i) {
        uct_obmm_region_close(&md->nc_regions[i]);
    }
    for (i = 0; i < md->num_cc_regions; ++i) {
        uct_obmm_region_close(&md->cc_regions[i]);
    }

    ucs_free(md->nc_regions);
    ucs_free(md->cc_regions);
    ucs_free(md->cc_exporters);

    md->nc_regions          = NULL;
    md->cc_regions          = NULL;
    md->cc_exporters        = NULL;
    md->num_nc_regions      = 0;
    md->num_cc_regions      = 0;
    md->num_cc_exporters    = 0;
    md->nc_export_idx       = -1;
    md->cc_export_idx       = -1;
    md->cc_exporter_index   = UINT16_MAX;
    md->cc_exporters_hash   = 0;
}


static void uct_obmm_md_close(uct_md_h tl_md)
{
    uct_obmm_md_t *md = ucs_derived_of(tl_md, uct_obmm_md_t);

    uct_obmm_md_unmap_all(md);
    ucs_free(md);
}


static ucs_status_t
uct_obmm_md_add_region(uct_obmm_region_t **regions_p, unsigned *count_p,
                       const uct_obmm_dev_info_t *info,
                       uct_obmm_region_mode_t region_mode, int *export_idx_p)
{
    uct_obmm_region_t *regions;
    ucs_status_t       status;
    unsigned           count = *count_p;

    regions = ucs_realloc(*regions_p, (count + 1) * sizeof(*regions),
                          "uct_obmm_regions");
    if (regions == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    *regions_p        = regions;
    regions[count].fd = -1;

    status = uct_obmm_region_open(info, region_mode, &regions[count]);
    if (status != UCS_OK) {
        ucs_warn("obmm: skipping %s memid=%" PRIu64 " device %s: %s",
                 (region_mode == UCT_OBMM_REGION_NC) ? "NC" : "CC",
                 info->memid, info->dev_path, ucs_status_string(status));
        return UCS_OK;
    }

    if (info->type == UCT_OBMM_DEV_EXPORT) {
        if (*export_idx_p >= 0) {
            ucs_error("obmm: multiple %s export regions in configured memids "
                      "(first memid=%" PRIu64 ", extra memid=%" PRIu64 ")",
                      (region_mode == UCT_OBMM_REGION_NC) ? "NC" : "CC",
                      regions[*export_idx_p].info.memid, info->memid);
            uct_obmm_region_close(&regions[count]);
            return UCS_ERR_INVALID_PARAM;
        }
        *export_idx_p = (int)count;
    }

    *count_p = count + 1;
    return UCS_OK;
}


static ucs_status_t
uct_obmm_md_map_devices(uct_obmm_md_t *md, const uct_obmm_dev_info_t *devs,
                        unsigned num_devs,
                        const uct_obmm_memid_list_t *nc_memids,
                        const uct_obmm_memid_list_t *cc_memids)
{
    ucs_status_t status;
    unsigned     i;

    for (i = 0; i < num_devs; ++i) {
        if (uct_obmm_memid_list_contains(nc_memids, devs[i].memid)) {
            status = uct_obmm_md_add_region(&md->nc_regions,
                                            &md->num_nc_regions, &devs[i],
                                            UCT_OBMM_REGION_NC,
                                            &md->nc_export_idx);
            if (status != UCS_OK) {
                return status;
            }
        }

        if (uct_obmm_memid_list_contains(cc_memids, devs[i].memid)) {
            status = uct_obmm_md_add_region(&md->cc_regions,
                                            &md->num_cc_regions, &devs[i],
                                            UCT_OBMM_REGION_CC,
                                            &md->cc_export_idx);
            if (status != UCS_OK) {
                return status;
            }
        }
    }

    if ((md->num_nc_regions == 0) || (md->nc_export_idx < 0)) {
        ucs_error("obmm: NC_MEMIDS must contain exactly one local export and "
                  "zero or more imports");
        return UCS_ERR_NO_DEVICE;
    }

    if (md->mode == UCT_OBMM_MEM_MODE_HYBRID) {
        if ((md->num_cc_regions == 0) || (md->cc_export_idx < 0)) {
            ucs_error("obmm: MEM_MODE=hybrid requires CC_MEMIDS with exactly "
                      "one local CC export and peer CC imports");
            return UCS_ERR_NO_DEVICE;
        }
    }

    return UCS_OK;
}


static int uct_obmm_exporter_cmp(const void *a, const void *b)
{
    const uct_obmm_exporter_id_t *ea = a;
    const uct_obmm_exporter_id_t *eb = b;

    if (ea->dcna != eb->dcna) {
        return (ea->dcna < eb->dcna) ? -1 : 1;
    }
    if (ea->deid.hi != eb->deid.hi) {
        return (ea->deid.hi < eb->deid.hi) ? -1 : 1;
    }
    if (ea->deid.lo != eb->deid.lo) {
        return (ea->deid.lo < eb->deid.lo) ? -1 : 1;
    }
    return 0;
}


static uint64_t uct_obmm_hash64_update(uint64_t hash, uint64_t value)
{
    unsigned i;

    for (i = 0; i < sizeof(value); ++i) {
        hash ^= (value >> (i * 8)) & 0xffu;
        hash *= 1099511628211ull;
    }
    return hash;
}


static ucs_status_t uct_obmm_md_build_cc_exporters(uct_obmm_md_t *md)
{
    uct_obmm_exporter_id_t *ids = NULL;
    uct_obmm_region_t      *r;
    uint64_t                hash = 1469598103934665603ull;
    unsigned                i, count = 0;
    ucs_status_t            status;

    if (md->mode != UCT_OBMM_MEM_MODE_HYBRID) {
        return UCS_OK;
    }

    for (i = 0; i < md->num_cc_regions; ++i) {
        r = &md->cc_regions[i];
        {
            uct_obmm_exporter_id_t *new_ids;

            new_ids = ucs_realloc(ids, (count + 1) * sizeof(*ids),
                                  "obmm_cc_exporters");
            if (new_ids == NULL) {
                ucs_free(ids);
                return UCS_ERR_NO_MEMORY;
            }
            ids = new_ids;
        }
        ids[count].dcna = r->info.exporter_dcna;
        ids[count].deid = r->info.exporter_deid;
        ++count;
    }

    qsort(ids, count, sizeof(*ids), uct_obmm_exporter_cmp);
    for (i = 1; i < count; ++i) {
        if (uct_obmm_exporter_cmp(&ids[i - 1], &ids[i]) == 0) {
            ucs_error("obmm: duplicate CC exporter tuple in configured "
                      "regions (dcna=0x%" PRIx64 " deid=0x%" PRIx64
                      ":0x%" PRIx64 ")",
                      ids[i].dcna, ids[i].deid.hi, ids[i].deid.lo);
            ucs_free(ids);
            return UCS_ERR_INVALID_PARAM;
        }
    }

    for (i = 0; i < count; ++i) {
        hash = uct_obmm_hash64_update(hash, ids[i].dcna);
        hash = uct_obmm_hash64_update(hash, ids[i].deid.hi);
        hash = uct_obmm_hash64_update(hash, ids[i].deid.lo);
    }

    md->cc_exporters      = ids;
    md->num_cc_exporters  = count;
    md->cc_exporters_hash = hash;

    r = uct_obmm_md_cc_export_region(md);
    status = uct_obmm_md_cc_exporter_index(md, r->info.exporter_dcna,
                                           &r->info.exporter_deid,
                                           &md->cc_exporter_index);
    if (status != UCS_OK) {
        ucs_error("obmm: local CC export is absent from exporter table");
        return status;
    }

    return UCS_OK;
}


static ucs_status_t
uct_obmm_md_validate_config(const uct_obmm_md_config_t *config,
                            uct_obmm_mem_mode_t *mode_p,
                            uct_obmm_memid_list_t *nc_memids,
                            uct_obmm_memid_list_t *cc_memids)
{
    ucs_status_t status;
    unsigned     i;

    status = uct_obmm_md_parse_mode(config->mem_mode, mode_p);
    if (status != UCS_OK) {
        return status;
    }

    status = uct_obmm_parse_memid_list(config->nc_memids, "NC_MEMIDS",
                                       nc_memids);
    if (status != UCS_OK) {
        return status;
    }
    status = uct_obmm_parse_memid_list(config->cc_memids, "CC_MEMIDS",
                                       cc_memids);
    if (status != UCS_OK) {
        return status;
    }

    if (nc_memids->count == 0) {
        ucs_error("obmm: UCX_OBMM_NC_MEMIDS is mandatory in v3");
        return UCS_ERR_INVALID_PARAM;
    }

    if ((*mode_p == UCT_OBMM_MEM_MODE_HYBRID) && (cc_memids->count == 0)) {
        ucs_error("obmm: UCX_OBMM_CC_MEMIDS is mandatory in MEM_MODE=hybrid");
        return UCS_ERR_INVALID_PARAM;
    }

    if ((*mode_p == UCT_OBMM_MEM_MODE_NC) && (cc_memids->count != 0)) {
        ucs_warn("obmm: UCX_OBMM_CC_MEMIDS is ignored in MEM_MODE=nc");
    }

    for (i = 0; i < nc_memids->count; ++i) {
        if (uct_obmm_memid_list_contains(cc_memids, nc_memids->values[i])) {
            ucs_error("obmm: memid %" PRIu64 " appears in both NC_MEMIDS and "
                      "CC_MEMIDS", nc_memids->values[i]);
            return UCS_ERR_INVALID_PARAM;
        }
    }

    return UCS_OK;
}


ucs_status_t uct_obmm_md_open(uct_component_t *component, const char *md_name,
                              const uct_md_config_t *tl_config,
                              uct_md_h *md_p)
{
    static uct_md_ops_t md_ops = {
        .close              = uct_obmm_md_close,
        .query              = uct_obmm_md_query,
        .mem_alloc          = uct_obmm_md_mem_alloc,
        .mem_free           = uct_obmm_md_mem_free,
        .mem_advise         = ucs_empty_function_return_unsupported,
        .mkey_pack          = ucs_empty_function_return_success,
        .mem_reg            = uct_md_dummy_mem_reg,
        .mem_dereg          = uct_md_dummy_mem_dereg,
        .mem_query          = ucs_empty_function_return_unsupported,
        .mem_attach         = ucs_empty_function_return_unsupported,
        .detect_memory_type = ucs_empty_function_return_unsupported
    };
    const uct_obmm_md_config_t *config = ucs_derived_of(tl_config,
                                                        uct_obmm_md_config_t);
    uct_obmm_memid_list_t       nc_memids = { NULL, 0 };
    uct_obmm_memid_list_t       cc_memids = { NULL, 0 };
    uct_obmm_memid_list_t       empty_memids = { NULL, 0 };
    uct_obmm_dev_info_t        *devs      = NULL;
    unsigned                    num_devs  = 0;
    uct_obmm_md_t              *md;
    ucs_status_t                status;

    (void)component;
    (void)md_name;

    md = ucs_calloc(1, sizeof(*md), "uct_obmm_md");
    if (md == NULL) {
        ucs_error("failed to allocate obmm md");
        return UCS_ERR_NO_MEMORY;
    }
    md->nc_export_idx     = -1;
    md->cc_export_idx     = -1;
    md->cc_exporter_index = UINT16_MAX;

    status = uct_obmm_md_validate_config(config, &md->mode, &nc_memids,
                                         &cc_memids);
    if (status != UCS_OK) {
        goto err_cleanup_lists;
    }

    status = uct_obmm_sysfs_discover(&devs, &num_devs);
    if (status != UCS_OK) {
        ucs_debug("obmm: sysfs discovery failed: %s",
                  ucs_status_string(status));
        goto err_cleanup_lists;
    }

    if (num_devs == 0) {
        ucs_debug("obmm: no shmdev devices found under " UCT_OBMM_SYSFS_ROOT);
        status = UCS_ERR_NO_DEVICE;
        goto err_free_devs;
    }

    status = uct_obmm_md_map_devices(md, devs, num_devs, &nc_memids,
                                     (md->mode == UCT_OBMM_MEM_MODE_HYBRID) ?
                                     &cc_memids : &empty_memids);
    if (status != UCS_OK) {
        goto err_unmap;
    }

    status = uct_obmm_md_build_cc_exporters(md);
    if (status != UCS_OK) {
        goto err_unmap;
    }

    uct_obmm_sysfs_release(devs);
    uct_obmm_memid_list_cleanup(&nc_memids);
    uct_obmm_memid_list_cleanup(&cc_memids);

    md->super.ops       = &md_ops;
    md->super.component = &uct_obmm_component;
    *md_p               = &md->super;
    return UCS_OK;

err_unmap:
    uct_obmm_md_unmap_all(md);
err_free_devs:
    uct_obmm_sysfs_release(devs);
err_cleanup_lists:
    uct_obmm_memid_list_cleanup(&nc_memids);
    uct_obmm_memid_list_cleanup(&cc_memids);
    ucs_free(md);
    return status;
}


uct_obmm_region_t *
uct_obmm_md_find_import_region(uct_obmm_md_t *md, uint64_t exporter_dcna,
                               const uct_obmm_eid_t *exporter_deid)
{
    unsigned i;

    for (i = 0; i < md->num_nc_regions; ++i) {
        uct_obmm_region_t *r = &md->nc_regions[i];

        if (r->info.type != UCT_OBMM_DEV_IMPORT) {
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
uct_obmm_md_find_cc_region(uct_obmm_md_t *md, uint64_t exporter_dcna,
                           const uct_obmm_eid_t *exporter_deid)
{
    unsigned i;

    for (i = 0; i < md->num_cc_regions; ++i) {
        uct_obmm_region_t *r = &md->cc_regions[i];

        if ((r->info.exporter_dcna == exporter_dcna) &&
            (r->info.exporter_deid.hi == exporter_deid->hi) &&
            (r->info.exporter_deid.lo == exporter_deid->lo)) {
            return r;
        }
    }
    return NULL;
}


uct_obmm_region_t *
uct_obmm_md_find_cc_region_by_index(uct_obmm_md_t *md, uint16_t index)
{
    uct_obmm_exporter_id_t *id;

    if ((md->mode != UCT_OBMM_MEM_MODE_HYBRID) ||
        (index >= md->num_cc_exporters)) {
        return NULL;
    }

    id = &md->cc_exporters[index];
    return uct_obmm_md_find_cc_region(md, id->dcna, &id->deid);
}


ucs_status_t uct_obmm_md_cc_exporter_index(uct_obmm_md_t *md,
                                           uint64_t exporter_dcna,
                                           const uct_obmm_eid_t *exporter_deid,
                                           uint16_t *index_p)
{
    unsigned i;

    for (i = 0; i < md->num_cc_exporters; ++i) {
        if ((md->cc_exporters[i].dcna == exporter_dcna) &&
            (md->cc_exporters[i].deid.hi == exporter_deid->hi) &&
            (md->cc_exporters[i].deid.lo == exporter_deid->lo)) {
            *index_p = (uint16_t)i;
            return UCS_OK;
        }
    }

    return UCS_ERR_NO_ELEM;
}


uct_obmm_region_t *uct_obmm_md_export_region(uct_obmm_md_t *md)
{
    if (md->nc_export_idx < 0) {
        return NULL;
    }
    return &md->nc_regions[md->nc_export_idx];
}


uct_obmm_region_t *uct_obmm_md_cc_export_region(uct_obmm_md_t *md)
{
    if (md->cc_export_idx < 0) {
        return NULL;
    }
    return &md->cc_regions[md->cc_export_idx];
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
