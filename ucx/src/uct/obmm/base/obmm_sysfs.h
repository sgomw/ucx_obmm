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


/**
 * Information about a single obmm shmdev as discovered from sysfs.
 *
 * For both export and import devices, (exporter_dcna, exporter_deid, memid)
 * uniquely identifies the underlying region across the cluster: it is what
 * goes on the wire in `uct_obmm_iface_addr_t` and what reachability matches
 * on. Specifically:
 *   - export: exporter_dcna = THIS host's clan network address (derived
 *     from any local import_info/scna), exporter_deid = our own
 *     export_info/deid, memid = local memid.
 *   - import: exporter_dcna = remote host's import_info/dcna,
 *     exporter_deid = remote host's import_info/deid, memid = local memid
 *     of the shmdev that mirrors that remote region.
 */
typedef struct uct_obmm_dev_info {
    uint64_t            memid;
    uint64_t            size;
    uct_obmm_dev_type_t type;
    uint64_t            exporter_dcna;
    uct_obmm_eid_t      exporter_deid;
    int                 allow_mmap;
    char                dev_path[UCT_OBMM_PATH_MAX];
} uct_obmm_dev_info_t;


/**
 * Scan /sys/devices/obmm and return the list of shmdevs available to this
 * process. Devices that are missing required attributes, are not mmap-able,
 * or fail to parse are silently skipped (with a debug log).
 *
 * The returned array is allocated via ucs_calloc and must be freed with
 * uct_obmm_sysfs_release().
 *
 * If no obmm devices are present, returns UCS_OK with *num_devices_p == 0
 * and *devices_p == NULL.
 */
ucs_status_t uct_obmm_sysfs_discover(uct_obmm_dev_info_t **devices_p,
                                     unsigned *num_devices_p);

void uct_obmm_sysfs_release(uct_obmm_dev_info_t *devices);


#endif
