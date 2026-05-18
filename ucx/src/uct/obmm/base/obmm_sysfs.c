/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "obmm_sysfs.h"

#include <ucs/debug/log.h>
#include <ucs/debug/memtrack_int.h>
#include <ucs/sys/sys.h>
#include <ucs/sys/string.h>

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>


typedef struct {
    uct_obmm_dev_info_t *devices;
    unsigned             count;
    unsigned             capacity;
} uct_obmm_sysfs_ctx_t;


static ucs_status_t uct_obmm_read_u64_hex(uint64_t *value, int silent,
                                          const char *path_fmt, ...)
    UCS_F_PRINTF(3, 4);

static ucs_status_t uct_obmm_read_u64_hex(uint64_t *value, int silent,
                                          const char *path_fmt, ...)
{
    char     buf[64];
    char     path[UCT_OBMM_PATH_MAX];
    va_list  ap;
    ssize_t  n;
    uint64_t v;

    va_start(ap, path_fmt);
    vsnprintf(path, sizeof(path), path_fmt, ap);
    va_end(ap);

    n = ucs_read_file_str(buf, sizeof(buf), silent, "%s", path);
    if (n < 0) {
        return UCS_ERR_IO_ERROR;
    }

    if (sscanf(buf, "%" SCNx64, &v) != 1) {
        if (!silent) {
            ucs_error("obmm: failed to parse u64 hex from %s: '%s'", path, buf);
        }
        return UCS_ERR_INVALID_PARAM;
    }

    *value = v;
    return UCS_OK;
}


static ucs_status_t uct_obmm_read_eid(uct_obmm_eid_t *eid, int silent,
                                      const char *path_fmt, ...)
    UCS_F_PRINTF(3, 4);

static ucs_status_t uct_obmm_read_eid(uct_obmm_eid_t *eid, int silent,
                                      const char *path_fmt, ...)
{
    char     buf[96];
    char     path[UCT_OBMM_PATH_MAX];
    va_list  ap;
    ssize_t  n;
    uint64_t hi, lo;

    va_start(ap, path_fmt);
    vsnprintf(path, sizeof(path), path_fmt, ap);
    va_end(ap);

    n = ucs_read_file_str(buf, sizeof(buf), silent, "%s", path);
    if (n < 0) {
        return UCS_ERR_IO_ERROR;
    }

    /* Format per obmm_shmdev_sysfs.md: "u64 : u64" with whitespace around ':'.
     * Accept either "0x%lx : 0x%lx" or bare hex with optional spaces. */
    if (sscanf(buf, "%" SCNx64 " : %" SCNx64, &hi, &lo) != 2) {
        if (!silent) {
            ucs_error("obmm: failed to parse eid (u64 : u64) from %s: '%s'",
                      path, buf);
        }
        return UCS_ERR_INVALID_PARAM;
    }

    eid->hi = hi;
    eid->lo = lo;
    return UCS_OK;
}


static int uct_obmm_parse_memid(const char *name, uint64_t *memid_p)
{
    const size_t prefix_len = sizeof(UCT_OBMM_SHMDEV_PREFIX) - 1;
    char        *end;
    uint64_t     memid;

    if (strncmp(name, UCT_OBMM_SHMDEV_PREFIX, prefix_len) != 0) {
        return 0;
    }

    if (name[prefix_len] == '\0') {
        return 0;
    }

    memid = strtoull(name + prefix_len, &end, 10);
    if ((*end != '\0') || (end == name + prefix_len)) {
        return 0;
    }

    if (memid == 0) {
        return 0;
    }

    *memid_p = memid;
    return 1;
}


static ucs_status_t
uct_obmm_sysfs_load_one(uct_obmm_dev_info_t *info, const char *dirname,
                        uint64_t memid)
{
    char           sysfs_dir[UCT_OBMM_PATH_MAX];
    char           type_buf[16];
    long           allow_mmap;
    ucs_status_t   status;
    ssize_t        n;

    ucs_snprintf_safe(sysfs_dir, sizeof(sysfs_dir), "%s/%s",
                      UCT_OBMM_SYSFS_ROOT, dirname);

    /* type */
    n = ucs_read_file_str(type_buf, sizeof(type_buf), 1, "%s/type", sysfs_dir);
    if (n < 0) {
        ucs_debug("obmm: %s: missing 'type'", sysfs_dir);
        return UCS_ERR_NO_ELEM;
    }
    ucs_strtrim(type_buf);

    if (!strcmp(type_buf, "export")) {
        info->type = UCT_OBMM_DEV_EXPORT;
    } else if (!strcmp(type_buf, "import")) {
        info->type = UCT_OBMM_DEV_IMPORT;
    } else {
        ucs_debug("obmm: %s: unknown type '%s'", sysfs_dir, type_buf);
        return UCS_ERR_NO_ELEM;
    }

    /* size (hex) */
    status = uct_obmm_read_u64_hex(&info->size, 1, "%s/size", sysfs_dir);
    if (status != UCS_OK) {
        ucs_debug("obmm: %s: missing/unreadable 'size'", sysfs_dir);
        return status;
    }

    /* allow_mmap (decimal) */
    status = ucs_read_file_number(&allow_mmap, 1, "%s/allow_mmap", sysfs_dir);
    if (status != UCS_OK) {
        ucs_debug("obmm: %s: missing 'allow_mmap'", sysfs_dir);
        return status;
    }
    info->allow_mmap = (allow_mmap != 0);

    if (!info->allow_mmap) {
        ucs_debug("obmm: %s: allow_mmap=0", sysfs_dir);
        return UCS_ERR_NO_ELEM;
    }

    info->memid = memid;
    ucs_snprintf_safe(info->dev_path, sizeof(info->dev_path),
                      UCT_OBMM_DEV_PATH_FMT, memid);

    if (info->type == UCT_OBMM_DEV_IMPORT) {
        /* exporter identity comes directly from import_info/{dcna,deid} */
        status = uct_obmm_read_u64_hex(&info->exporter_dcna, 1,
                                       "%s/import_info/dcna", sysfs_dir);
        if (status != UCS_OK) {
            ucs_debug("obmm: %s: missing import_info/dcna", sysfs_dir);
            return status;
        }
        status = uct_obmm_read_eid(&info->exporter_deid, 1,
                                   "%s/import_info/deid", sysfs_dir);
        if (status != UCS_OK) {
            ucs_debug("obmm: %s: missing import_info/deid", sysfs_dir);
            return status;
        }
    } else {
        /* For exports, exporter_deid is in export_info/deid; exporter_dcna
         * is THIS host's clan network address and is filled by a second
         * pass once we have observed any import device's import_info/scna.
         * Leave it at 0 for now. */
        status = uct_obmm_read_eid(&info->exporter_deid, 1,
                                   "%s/export_info/deid", sysfs_dir);
        if (status != UCS_OK) {
            ucs_debug("obmm: %s: missing export_info/deid", sysfs_dir);
            return status;
        }
        info->exporter_dcna = 0;
    }

    return UCS_OK;
}


static ucs_status_t uct_obmm_sysfs_grow(uct_obmm_sysfs_ctx_t *ctx)
{
    uct_obmm_dev_info_t *new_arr;
    unsigned             new_cap;

    if (ctx->count < ctx->capacity) {
        return UCS_OK;
    }

    new_cap = (ctx->capacity == 0) ? 8 : (ctx->capacity * 2);
    new_arr = ucs_realloc(ctx->devices, new_cap * sizeof(*new_arr),
                          "uct_obmm_dev_info");
    if (new_arr == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    ctx->devices  = new_arr;
    ctx->capacity = new_cap;
    return UCS_OK;
}


static ucs_status_t
uct_obmm_sysfs_fail_status(ucs_status_t status)
{
    return (status == UCS_ERR_NO_ELEM) ? UCS_ERR_UNSUPPORTED : status;
}


static ucs_status_t
uct_obmm_sysfs_append_entry(uct_obmm_sysfs_ctx_t *ctx, const char *dirname,
                            uint64_t memid)
{
    uct_obmm_dev_info_t *info;
    ucs_status_t         status;

    status = uct_obmm_sysfs_grow(ctx);
    if (status != UCS_OK) {
        return status;
    }

    info = &ctx->devices[ctx->count];
    memset(info, 0, sizeof(*info));

    status = uct_obmm_sysfs_load_one(info, dirname, memid);
    if (status != UCS_OK) {
        ucs_debug("obmm: shmdev memid=%" PRIu64
                  " is unavailable or unusable for obmm discovery",
                  memid);
        return uct_obmm_sysfs_fail_status(status);
    }

    ctx->count++;
    return UCS_OK;
}


static ucs_status_t uct_obmm_sysfs_readdir_cb(const struct dirent *entry,
                                              void *arg)
{
    uct_obmm_sysfs_ctx_t *ctx = (uct_obmm_sysfs_ctx_t*)arg;
    uint64_t              memid;

    if (!uct_obmm_parse_memid(entry->d_name, &memid)) {
        return UCS_OK; /* not a shmdev entry */
    }

    return uct_obmm_sysfs_append_entry(ctx, entry->d_name, memid);
}


/* After the first pass we may know our own clan network address (scna) from
 * any import device. Patch it into export entries' exporter_dcna so they
 * become uniquely identifiable across the cluster. */
static ucs_status_t uct_obmm_sysfs_patch_self_dcna(uct_obmm_sysfs_ctx_t *ctx)
{
    uint64_t      self_dcna = 0;
    int           have_self = 0;
    int           have_export = 0;
    ucs_status_t  status;
    char          sysfs_dir[UCT_OBMM_PATH_MAX];
    unsigned      i;

    for (i = 0; i < ctx->count; ++i) {
        if (ctx->devices[i].type == UCT_OBMM_DEV_EXPORT) {
            have_export = 1;
        }
        if (ctx->devices[i].type != UCT_OBMM_DEV_IMPORT) {
            continue;
        }
        ucs_snprintf_safe(sysfs_dir, sizeof(sysfs_dir), "%s/%s%" PRIu64,
                          UCT_OBMM_SYSFS_ROOT, UCT_OBMM_SHMDEV_PREFIX,
                          ctx->devices[i].memid);
        status = uct_obmm_read_u64_hex(&self_dcna, 1, "%s/import_info/scna",
                                       sysfs_dir);
        if (status == UCS_OK) {
            have_self = 1;
            break;
        }
    }

    if (!have_self) {
        if (have_export) {
            ucs_debug("obmm: an export region exists but no import region is "
                      "available to derive self dcna");
            return UCS_ERR_NO_DEVICE;
        }
        ucs_debug("obmm: no import device available to derive self dcna; "
                  "export entries will keep exporter_dcna=0");
        return UCS_OK;
    }

    for (i = 0; i < ctx->count; ++i) {
        if (ctx->devices[i].type == UCT_OBMM_DEV_EXPORT) {
            ctx->devices[i].exporter_dcna = self_dcna;
        }
    }
    return UCS_OK;
}


static ucs_status_t
uct_obmm_sysfs_discover_filtered(uct_obmm_sysfs_ctx_t *ctx,
                                 const uint64_t *filter_memids,
                                 unsigned num_filter_memids)
{
    char          dirname[64];
    ucs_status_t  status;
    unsigned      i;

    for (i = 0; i < num_filter_memids; ++i) {
        ucs_snprintf_safe(dirname, sizeof(dirname), "%s%" PRIu64,
                          UCT_OBMM_SHMDEV_PREFIX, filter_memids[i]);
        status = uct_obmm_sysfs_append_entry(ctx, dirname, filter_memids[i]);
        if (status != UCS_OK) {
            return status;
        }
    }

    return UCS_OK;
}


ucs_status_t uct_obmm_sysfs_discover(uct_obmm_dev_info_t **devices_p,
                                     unsigned *num_devices_p,
                                     const uint64_t *filter_memids,
                                     unsigned num_filter_memids)
{
    uct_obmm_sysfs_ctx_t ctx = { NULL, 0, 0 };
    ucs_status_t         status;

    if (num_filter_memids > 0) {
        status = uct_obmm_sysfs_discover_filtered(&ctx, filter_memids,
                                                  num_filter_memids);
    } else {
        status = ucs_sys_readdir(UCT_OBMM_SYSFS_ROOT, uct_obmm_sysfs_readdir_cb,
                                 &ctx);
    }

    if (status != UCS_OK) {
        if (num_filter_memids == 0) {
            ucs_debug("obmm: failed to scan %s: %s", UCT_OBMM_SYSFS_ROOT,
                      ucs_status_string(status));
        }
        ucs_free(ctx.devices);
        *devices_p     = NULL;
        *num_devices_p = 0;
        /* Treat "no obmm at all" as success-with-zero so md_open can decide. */
        return (status == UCS_ERR_NO_ELEM) ? UCS_OK : status;
    }

    if (ctx.count == 0) {
        ucs_free(ctx.devices);
        *devices_p     = NULL;
        *num_devices_p = 0;
        return UCS_OK;
    }

    status = uct_obmm_sysfs_patch_self_dcna(&ctx);
    if (status != UCS_OK) {
        ucs_free(ctx.devices);
        *devices_p     = NULL;
        *num_devices_p = 0;
        return status;
    }

    *devices_p     = ctx.devices;
    *num_devices_p = ctx.count;
    return UCS_OK;
}


void uct_obmm_sysfs_release(uct_obmm_dev_info_t *devices)
{
    ucs_free(devices);
}
