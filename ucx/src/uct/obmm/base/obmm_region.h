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


/**
 * A live mapping of one obmm shmdev region. The fd is kept open for the
 * lifetime of the mapping (kernel may require it for syncing / liveness).
 *
 * NC mappings are created with O_SYNC + MAP_SHARED so writes from this host
 * are non-cacheable and visible to remote hosts without ownership flips.
 * Same-node CC mappings intentionally omit O_SYNC so the exported cacheable
 * memory can be used as the fast local shared-memory plane.
 */
typedef struct uct_obmm_region {
    uct_obmm_dev_info_t info;       /* sysfs-derived metadata        */
    int                 fd;         /* open shmdev fd                */
    void               *base;       /* mmap base, length = info.size */
    size_t              length;
} uct_obmm_region_t;


/**
 * Open + mmap the region described by `info`. On success the caller owns
 * the mapping and must release it via uct_obmm_region_close().
 */
ucs_status_t uct_obmm_region_open(const uct_obmm_dev_info_t *info,
                                  uct_obmm_region_t *region);

void uct_obmm_region_close(uct_obmm_region_t *region);


#endif
