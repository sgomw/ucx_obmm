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

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#define UCT_OBMM_PRIV_MAX 512
#define UCT_OBMM_UB_DEVICES_ROOT "/sys/devices"
#define UCT_OBMM_UB_CONTROLLER_PREFIX "ub_bus_controller"


static int
uct_obmm_sysfs_parse_transport_priv(const char *priv, long priv_len,
                                    uint32_t *region_id_p);


static ucs_status_t
uct_obmm_sysfs_read_priv(char *priv, size_t max, long *priv_len_p,
                         const char *sysfs_dir)
{
    ucs_status_t status;
    ssize_t      nread;

    status = ucs_read_file_number(priv_len_p, 1, "%s/priv_len", sysfs_dir);
    if (status != UCS_OK) {
        return UCS_ERR_NO_ELEM;
    }

    if ((*priv_len_p < 0) || (*priv_len_p > UCT_OBMM_PRIV_MAX)) {
        ucs_debug("obmm: %s: invalid priv_len=%ld", sysfs_dir, *priv_len_p);
        return UCS_ERR_INVALID_PARAM;
    }

    if (*priv_len_p == 0) {
        return UCS_OK;
    }

    nread = ucs_read_file(priv, max, 1, "%s/priv", sysfs_dir);
    if (nread < *priv_len_p) {
        ucs_debug("obmm: %s: priv read returned %zd bytes, expected %ld",
                  sysfs_dir, nread, *priv_len_p);
        return UCS_ERR_IO_ERROR;
    }

    return UCS_OK;
}


static int uct_obmm_sysfs_priv_is_transport(const char *sysfs_dir)
{
    char         priv[UCT_OBMM_PRIV_MAX + 1];
    long         priv_len;
    ucs_status_t status;

    status = uct_obmm_sysfs_read_priv(priv, sizeof(priv), &priv_len,
                                      sysfs_dir);
    if (status != UCS_OK) {
        return 0;
    }

    return uct_obmm_sysfs_parse_transport_priv(priv, priv_len, NULL);
}


static int
uct_obmm_sysfs_parse_transport_priv(const char *priv, long priv_len,
                                    uint32_t *region_id_p)
{
    uint32_t region_id;
    size_t   prefix_len;

    prefix_len = strlen(UCT_OBMM_PRIV_PREFIX);
    if (priv_len != (long)(prefix_len + UCT_OBMM_PRIV_INDEX_LEN)) {
        return 0;
    }

    if (memcmp(priv, UCT_OBMM_PRIV_PREFIX, prefix_len) ||
        !isdigit((unsigned char)priv[prefix_len]) ||
        !isdigit((unsigned char)priv[prefix_len + 1])) {
        return 0;
    }

    region_id = ((uint32_t)(priv[prefix_len] - '0') * 10u) +
                (uint32_t)(priv[prefix_len + 1] - '0');
    if (region_id_p != NULL) {
        *region_id_p = region_id;
    }
    return 1;
}


static int uct_obmm_sysfs_parse_shmdev_memid(const char *dirname,
                                             uint64_t *memid_p)
{
    const char *suffix;
    char       *end;
    uint64_t    memid;

    if (strncmp(dirname, UCT_OBMM_SHMDEV_PREFIX,
                strlen(UCT_OBMM_SHMDEV_PREFIX))) {
        return 0;
    }

    suffix = dirname + strlen(UCT_OBMM_SHMDEV_PREFIX);
    if (*suffix == '\0') {
        return 0;
    }

    errno = 0;
    memid = strtoull(suffix, &end, 10);
    if ((errno == ERANGE) || (*end != '\0') || (memid == 0)) {
        return 0;
    }

    *memid_p = memid;
    return 1;
}


static int uct_obmm_sysfs_compare_memid(const void *a, const void *b)
{
    const uct_obmm_dev_info_t *da = a;
    const uct_obmm_dev_info_t *db = b;

    if (da->memid < db->memid) {
        return -1;
    } else if (da->memid > db->memid) {
        return 1;
    }
    return 0;
}


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


static ucs_status_t uct_obmm_read_deid(uint32_t *eid, int silent,
                                       const char *path_fmt, ...)
    UCS_F_PRINTF(3, 4);


static ucs_status_t uct_obmm_read_ctl_int_base0(uint32_t *value, int silent,
                                                const char *path_fmt, ...)
    UCS_F_PRINTF(3, 4);


static ucs_status_t uct_obmm_read_ctl_int_base0(uint32_t *value, int silent,
                                                const char *path_fmt, ...)
{
    char     buf[64];
    char     path[UCT_OBMM_PATH_MAX];
    char    *end;
    va_list  ap;
    ssize_t  n;
    long     v;

    va_start(ap, path_fmt);
    vsnprintf(path, sizeof(path), path_fmt, ap);
    va_end(ap);

    n = ucs_read_file_str(buf, sizeof(buf), silent, "%s", path);
    if (n < 0) {
        return UCS_ERR_IO_ERROR;
    }

    ucs_strtrim(buf);
    errno = 0;
    v = strtol(buf, &end, 0);
    if ((end == buf) || (*end != '\0') || (errno != 0) ||
        (v < 0) || (v > INT_MAX)) {
        if (!silent) {
            ucs_error("obmm: failed to parse controller int from %s: '%s'",
                      path, buf);
        }
        return UCS_ERR_INVALID_PARAM;
    }

    *value = (uint32_t)v;
    return UCS_OK;
}


static ucs_status_t uct_obmm_read_deid(uint32_t *eid, int silent,
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

    if ((hi != 0) || (lo > UCT_OBMM_EID_MAX)) {
        if (!silent) {
            ucs_error("obmm: invalid obmm deid from %s: "
                      "0x%" PRIx64 ":0x%" PRIx64 " (max low %u bits)",
                      path, hi, lo, UCT_OBMM_EID_BITS);
        }
        return UCS_ERR_INVALID_PARAM;
    }

    *eid = (uint32_t)lo;
    return UCS_OK;
}


static ucs_status_t
uct_obmm_sysfs_load_region_id(uint32_t *region_id_p, const char *sysfs_dir)
{
    char         priv[UCT_OBMM_PRIV_MAX + 1];
    long         priv_len;
    ucs_status_t status;

    status = uct_obmm_sysfs_read_priv(priv, sizeof(priv), &priv_len,
                                      sysfs_dir);
    if (status != UCS_OK) {
        return status;
    }

    if (uct_obmm_sysfs_parse_transport_priv(priv, priv_len, region_id_p)) {
        return UCS_OK;
    }

    ucs_error("obmm: %s: unsupported private metadata '%.*s'; expected "
              UCT_OBMM_PRIV_PREFIX "NN",
              sysfs_dir, (int)priv_len, priv);
    return UCS_ERR_INVALID_PARAM;
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
        status = uct_obmm_read_deid(&info->exporter_deid, 1,
                                    "%s/import_info/deid", sysfs_dir);
        if (status != UCS_OK) {
            ucs_debug("obmm: %s: missing/invalid import_info/deid",
                      sysfs_dir);
            return status;
        }
    } else {
        /* For exports, exporter_deid is in export_info/deid; exporter_dcna
         * is THIS host's clan network address and is filled by the MD from
         * the local controller identity. Leave it at 0 for now. */
        status = uct_obmm_read_deid(&info->exporter_deid, 1,
                                    "%s/export_info/deid", sysfs_dir);
        if (status != UCS_OK) {
            ucs_debug("obmm: %s: missing/invalid export_info/deid",
                      sysfs_dir);
            return status;
        }
        info->exporter_dcna = 0;
    }

    status = uct_obmm_sysfs_load_region_id(&info->region_id, sysfs_dir);
    if (status != UCS_OK) {
        return status;
    }

    return UCS_OK;
}


static ucs_status_t
uct_obmm_sysfs_append_auto_device(uct_obmm_dev_info_t **devices_p,
                                  unsigned *num_devices_p,
                                  unsigned *capacity_p,
                                  const char *dirname, uint64_t memid)
{
    uct_obmm_dev_info_t *devices;
    ucs_status_t         status;
    unsigned             new_capacity;

    if (*num_devices_p == *capacity_p) {
        new_capacity = (*capacity_p == 0) ? 16 : (*capacity_p * 2);
        devices = ucs_realloc(*devices_p,
                              new_capacity * sizeof(**devices_p),
                              "uct_obmm_auto_dev_info");
        if (devices == NULL) {
            return UCS_ERR_NO_MEMORY;
        }

        memset(devices + *capacity_p, 0,
               (new_capacity - *capacity_p) * sizeof(*devices));
        *devices_p  = devices;
        *capacity_p = new_capacity;
    }

    status = uct_obmm_sysfs_load_one(&(*devices_p)[*num_devices_p], dirname,
                                     memid);
    if (status != UCS_OK) {
        ucs_debug("obmm: shmdev memid=%" PRIu64
                  " has transport priv but is unusable for obmm discovery",
                  memid);
        return status;
    }

    ++(*num_devices_p);
    return UCS_OK;
}


static ucs_status_t
uct_obmm_sysfs_discover_auto(uct_obmm_dev_info_t **devices_p,
                             unsigned *num_devices_p)
{
    uct_obmm_dev_info_t *devices = NULL;
    unsigned             num_devices = 0;
    unsigned             capacity = 0;
    char                 sysfs_dir[UCT_OBMM_PATH_MAX];
    uint64_t             memid;
    ucs_status_t         status;
    struct dirent       *entry;
    DIR                 *dir;

    dir = opendir(UCT_OBMM_SYSFS_ROOT);
    if (dir == NULL) {
        ucs_debug("obmm: failed to open %s: %m", UCT_OBMM_SYSFS_ROOT);
        return UCS_ERR_NO_DEVICE;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (!uct_obmm_sysfs_parse_shmdev_memid(entry->d_name, &memid)) {
            continue;
        }

        ucs_snprintf_safe(sysfs_dir, sizeof(sysfs_dir), "%s/%s",
                          UCT_OBMM_SYSFS_ROOT, entry->d_name);
        if (!uct_obmm_sysfs_priv_is_transport(sysfs_dir)) {
            continue;
        }

        status = uct_obmm_sysfs_append_auto_device(&devices, &num_devices,
                                                   &capacity, entry->d_name,
                                                   memid);
        if (status != UCS_OK) {
            goto err_closedir;
        }
    }

    closedir(dir);

    if (num_devices == 0) {
        ucs_free(devices);
        return UCS_ERR_NO_DEVICE;
    }

    qsort(devices, num_devices, sizeof(*devices),
          uct_obmm_sysfs_compare_memid);

    *devices_p     = devices;
    *num_devices_p = num_devices;
    return UCS_OK;

err_closedir:
    closedir(dir);
    ucs_free(devices);
    return status;
}


ucs_status_t uct_obmm_sysfs_discover(uct_obmm_dev_info_t **devices_p,
                                     unsigned *num_devices_p)
{
    *devices_p = NULL;
    *num_devices_p = 0;
    return uct_obmm_sysfs_discover_auto(devices_p, num_devices_p);
}


void uct_obmm_sysfs_release(uct_obmm_dev_info_t *devices)
{
    ucs_free(devices);
}


void uct_obmm_sysfs_set_exporter_cna(uct_obmm_dev_info_t *devices,
                                     unsigned num_devices, uint64_t self_cna)
{
    unsigned i;

    for (i = 0; i < num_devices; ++i) {
        if (devices[i].type == UCT_OBMM_DEV_EXPORT) {
            devices[i].exporter_dcna = self_cna;
        }
    }
}


static int uct_obmm_sysfs_is_dot_dir(const char *name)
{
    return !strcmp(name, ".") || !strcmp(name, "..");
}


static int uct_obmm_sysfs_has_prefix(const char *name, const char *prefix)
{
    return strncmp(name, prefix, strlen(prefix)) == 0;
}


static int uct_obmm_sysfs_has_attr(const char *sysfs_dir, const char *attr)
{
    char path[UCT_OBMM_PATH_MAX];

    ucs_snprintf_safe(path, sizeof(path), "%s/%s", sysfs_dir, attr);
    return access(path, R_OK) == 0;
}


static int uct_obmm_sysfs_has_ubc_marker(const char *sysfs_dir)
{
    char path[UCT_OBMM_PATH_MAX];

    ucs_snprintf_safe(path, sizeof(path), "%s/ubc", sysfs_dir);
    return access(path, F_OK) == 0;
}


static ucs_status_t
uct_obmm_sysfs_try_local_identity(const char *sysfs_dir,
                                  uint64_t *cna_p,
                                  uint32_t *eid_p)
{
    uint32_t     cna, eid_lo;
    ucs_status_t status;

    status = uct_obmm_read_ctl_int_base0(&eid_lo, 1, "%s/eid", sysfs_dir);
    if (status != UCS_OK) {
        ucs_debug("obmm: %s: missing/unreadable local controller eid",
                  sysfs_dir);
        return status;
    }

    status = uct_obmm_read_ctl_int_base0(&cna, 1, "%s/primary_cna",
                                         sysfs_dir);
    if (status != UCS_OK) {
        ucs_debug("obmm: %s: missing/unreadable local controller primary_cna",
                  sysfs_dir);
        return status;
    }

    if (eid_lo > UCT_OBMM_EID_MAX) {
        ucs_debug("obmm: %s: local controller eid 0x%x exceeds %u bits",
                  sysfs_dir, eid_lo, UCT_OBMM_EID_BITS);
        return UCS_ERR_INVALID_PARAM;
    }

    *cna_p = cna;
    *eid_p = eid_lo;
    return UCS_OK;
}


ucs_status_t uct_obmm_sysfs_read_local_identity(uint64_t *cna_p,
                                                uint32_t *eid_p)
{
    struct dirent *ctrl_entry;
    struct dirent *dev_entry;
    char           ctrl_dir_path[UCT_OBMM_PATH_MAX];
    char           dev_dir_path[UCT_OBMM_PATH_MAX];
    ucs_status_t   status = UCS_ERR_NO_DEVICE;
    DIR           *root_dir;
    DIR           *ctrl_dir;

    root_dir = opendir(UCT_OBMM_UB_DEVICES_ROOT);
    if (root_dir == NULL) {
        ucs_debug("obmm: failed to open %s: %m", UCT_OBMM_UB_DEVICES_ROOT);
        return UCS_ERR_NO_DEVICE;
    }

    while ((ctrl_entry = readdir(root_dir)) != NULL) {
        if (!uct_obmm_sysfs_has_prefix(ctrl_entry->d_name,
                                       UCT_OBMM_UB_CONTROLLER_PREFIX)) {
            continue;
        }

        ucs_snprintf_safe(ctrl_dir_path, sizeof(ctrl_dir_path), "%s/%s",
                          UCT_OBMM_UB_DEVICES_ROOT, ctrl_entry->d_name);

        ctrl_dir = opendir(ctrl_dir_path);
        if (ctrl_dir == NULL) {
            continue;
        }

        while ((dev_entry = readdir(ctrl_dir)) != NULL) {
            if (uct_obmm_sysfs_is_dot_dir(dev_entry->d_name)) {
                continue;
            }

            ucs_snprintf_safe(dev_dir_path, sizeof(dev_dir_path), "%s/%s",
                              ctrl_dir_path, dev_entry->d_name);
            if (!uct_obmm_sysfs_has_ubc_marker(dev_dir_path)) {
                continue;
            }

            status = uct_obmm_sysfs_try_local_identity(dev_dir_path, cna_p,
                                                       eid_p);
            if (status == UCS_OK) {
                closedir(ctrl_dir);
                closedir(root_dir);
                return UCS_OK;
            }
        }

        closedir(ctrl_dir);
    }

    closedir(root_dir);
    if (status != UCS_ERR_NO_DEVICE) {
        ucs_debug("obmm: local UB controller identity candidates found "
                  "under %s but none were usable: %s",
                  UCT_OBMM_UB_DEVICES_ROOT, ucs_status_string(status));
        return status;
    }

    ucs_debug("obmm: no local UB controller identity found under %s",
              UCT_OBMM_UB_DEVICES_ROOT);
    return UCS_ERR_NO_DEVICE;
}
