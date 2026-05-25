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


typedef enum uct_obmm_map_mode {
    UCT_OBMM_MAP_MODE_NC = 0,
    UCT_OBMM_MAP_MODE_CC = 1
} uct_obmm_map_mode_t;


/**
 * A live mapping of one obmm shmdev region. The fd is kept open for the
 * lifetime of the mapping (kernel may require it for syncing / liveness).
 *
 * NC mappings use O_SYNC + MAP_SHARED so writes from this host are
 * non-cacheable and visible to remote hosts without obmm_set_ownership()
 * flips. CC mappings use plain O_RDWR + MAP_SHARED; import mappings start as
 * PROT_NONE so future ownership-aware paths can explicitly acquire access.
 */
typedef struct uct_obmm_region {
    uct_obmm_dev_info_t info;       /* sysfs-derived metadata        */
    uct_obmm_map_mode_t map_mode;   /* NC or CC mapping mode         */
    int                 fd;         /* open(dev_path, mode-specific) */
    void               *base;       /* mmap base, length = info.size */
    size_t              length;
} uct_obmm_region_t;


/**
 * Open + mmap the region described by `info`. On success the caller owns
 * the mapping and must release it via uct_obmm_region_close().
 */
ucs_status_t uct_obmm_region_open(const uct_obmm_dev_info_t *info,
                                  uct_obmm_map_mode_t map_mode,
                                  uct_obmm_region_t *region);

void uct_obmm_region_close(uct_obmm_region_t *region);


#endif
