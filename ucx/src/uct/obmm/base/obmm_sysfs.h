/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_SYSFS_H_
#define UCT_OBMM_SYSFS_H_

#include <ucs/type/status.h>
#include <stddef.h>
#include <stdint.h>


#define UCT_OBMM_SYSFS_ROOT       "/sys/devices/obmm"
#define UCT_OBMM_SHMDEV_PREFIX    "obmm_shmdev"
#define UCT_OBMM_PRIV_PREFIX      "ucx-obmm:"
#define UCT_OBMM_PRIV_INDEX_LEN   2
#define UCT_OBMM_DEV_PATH_FMT     "/dev/obmm_shmdev%lu"
#define UCT_OBMM_PATH_MAX         256


/* OBMM bus controller entity id, printed by sysfs as "u64 : u64" */
typedef struct uct_obmm_eid {
    uint64_t hi;
    uint64_t lo;
} uct_obmm_eid_t;


typedef enum {
    UCT_OBMM_DEV_EXPORT = 0,
    UCT_OBMM_DEV_IMPORT = 1
} uct_obmm_dev_type_t;

typedef enum {
    UCT_OBMM_PLANE_NC = 0,
    UCT_OBMM_PLANE_CC = 1,
    UCT_OBMM_PLANE_LAST
} uct_obmm_plane_t;


/**
 * Information about a single obmm shmdev as discovered from sysfs.
 *
 * For both export and import devices, (exporter_dcna, exporter_deid,
 * region_id) identifies the underlying region across the cluster: it is
 * carried in the OBMM device/iface addresses and matched by reachability.
 * region_id is derived from the sysfs private metadata when present; a zero
 * value means no private region id was available and is accepted only when the
 * mapped region list is otherwise unambiguous. Specifically:
 *   - export: exporter_dcna = THIS host's clan network address (derived
 *     from any local import_info/scna), exporter_deid = our own
 *     export_info/deid, region_id = hash(priv) or 0.
 *   - import: exporter_dcna = remote host's import_info/dcna,
 *     exporter_deid = remote host's import_info/deid,
 *     region_id = hash(priv) or 0.
 */
typedef struct uct_obmm_dev_info {
    uint64_t            memid;
    uint64_t            size;
    uct_obmm_dev_type_t type;
    uct_obmm_plane_t    plane;
    uint64_t            exporter_dcna;
    uct_obmm_eid_t      exporter_deid;
    uint32_t            region_id;
    int                 allow_mmap;
    char                dev_path[UCT_OBMM_PATH_MAX];
} uct_obmm_dev_info_t;


/**
 * Discover obmm shmdevs available to this process.
 *
 * If @a filter_memids is non-NULL and @a num_filter_memids is nonzero, only
 * those explicit memids are queried. Any requested memid that is missing or
 * unusable aborts discovery with error.
 *
 * If @a filter_memids is NULL or @a num_filter_memids is zero, all
 * /sys/devices/obmm/obmm_shmdev* directories are scanned and only devices
 * whose private metadata is exactly "ucx-obmm:NN" are returned. This automatic
 * mode is the normal block-FIFO NC discovery path.
 *
 * The returned array is allocated via ucs_calloc and must be freed with
 * uct_obmm_sysfs_release().
 *
 * On success, *devices_p contains one entry for every discovered device and
 * *num_devices_p contains the entry count.
 */
ucs_status_t uct_obmm_sysfs_discover(uct_obmm_dev_info_t **devices_p,
                                     unsigned *num_devices_p,
                                     const uint64_t *filter_memids,
                                     unsigned num_filter_memids);

void uct_obmm_sysfs_release(uct_obmm_dev_info_t *devices);


#endif
