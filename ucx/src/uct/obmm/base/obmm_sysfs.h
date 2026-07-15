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


#define UCT_OBMM_EID_BITS      20u
#define UCT_OBMM_EID_MAX       ((1u << UCT_OBMM_EID_BITS) - 1u)


typedef enum {
    UCT_OBMM_DEV_EXPORT = 0,
    UCT_OBMM_DEV_IMPORT = 1
} uct_obmm_dev_type_t;


/**
 * Information about a single obmm shmdev as discovered from sysfs.
 *
 * For both export and import devices, (exporter_dcna, exporter_deid,
 * region_id) identifies the underlying region across the cluster: it is
 * carried in the OBMM device/iface addresses and matched by reachability.
 * region_id is parsed from "ucx-obmm:NN" private metadata.
 * Specifically:
 *   - export: exporter_dcna = THIS host's clan network address (derived
 *     from the local controller identity), exporter_deid = our own
 *     export_info/deid, region_id = NN.
 *   - import: exporter_dcna = remote host's import_info/dcna,
 *     exporter_deid = remote host's import_info/deid,
 *     region_id = NN.
 */
typedef struct uct_obmm_dev_info {
    uint64_t            memid;
    uint64_t            size;
    uct_obmm_dev_type_t type;
    uint64_t            exporter_dcna;
    uint32_t            exporter_deid;
    uint32_t            region_id;
    int                 allow_mmap;
    char                dev_path[UCT_OBMM_PATH_MAX];
} uct_obmm_dev_info_t;


/**
 * Discover obmm shmdevs available to this process.
 *
 * All /sys/devices/obmm/obmm_shmdev* directories are scanned and only devices
 * whose private metadata is exactly "ucx-obmm:NN" are returned.
 *
 * The returned array is allocated via ucs_calloc and must be freed with
 * uct_obmm_sysfs_release().
 *
 * On success, *devices_p contains one entry for every discovered device and
 * *num_devices_p contains the entry count.
 */
ucs_status_t uct_obmm_sysfs_discover(uct_obmm_dev_info_t **devices_p,
                                     unsigned *num_devices_p);

ucs_status_t uct_obmm_sysfs_read_local_identity(uint64_t *cna_p,
                                                uint32_t *eid_p);

void uct_obmm_sysfs_set_exporter_cna(uct_obmm_dev_info_t *devices,
                                     unsigned num_devices,
                                     uint64_t self_cna);

void uct_obmm_sysfs_release(uct_obmm_dev_info_t *devices);


#endif
