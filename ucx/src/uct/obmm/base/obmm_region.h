/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_REGION_H_
#define UCT_OBMM_REGION_H_

#include "obmm_sysfs.h"

#include <ucs/type/status.h>
#include <stddef.h>


typedef enum {
    UCT_OBMM_REGION_NC = 0,
    UCT_OBMM_REGION_CC = 1
} uct_obmm_region_mode_t;

/**
 * A live mapping of one obmm shmdev region. The fd is kept open for the
 * lifetime of the mapping (kernel may require it for syncing / liveness).
 *
 * NC mappings are opened with O_SYNC and mmap'd RW. In static-CC hybrid mode,
 * local CC exports are mmap'd RW and peer CC imports are mmap'd read-only.
 */
typedef struct uct_obmm_region {
    uct_obmm_dev_info_t info;       /* sysfs-derived metadata        */
    int                 fd;         /* live shmdev fd                */
    void               *base;       /* mmap base, length = info.size */
    size_t              length;
    uct_obmm_region_mode_t mode;
} uct_obmm_region_t;


/**
 * Open + mmap the region described by `info`. On success the caller owns
 * the mapping and must release it via uct_obmm_region_close().
 */
ucs_status_t uct_obmm_region_open(const uct_obmm_dev_info_t *info,
                                  uct_obmm_region_mode_t mode,
                                  uct_obmm_region_t *region);

void uct_obmm_region_close(uct_obmm_region_t *region);

#endif
