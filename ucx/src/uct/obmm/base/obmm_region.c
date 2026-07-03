/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "obmm_region.h"

#include <ucs/debug/log.h>

#include <fcntl.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <unistd.h>


ucs_status_t uct_obmm_region_open(const uct_obmm_dev_info_t *info,
                                  uct_obmm_region_t *region)
{
    ucs_status_t status;
    void        *map;
    int          open_flags;
    int          fd;

    if (!info->allow_mmap) {
        ucs_debug("obmm: device %s does not support mmap", info->dev_path);
        return UCS_ERR_UNSUPPORTED;
    }

    if (info->size == 0) {
        ucs_debug("obmm: device %s reports zero size", info->dev_path);
        return UCS_ERR_NO_RESOURCE;
    }

    /* O_SYNC selects the non-cacheable mapping required by the cross-node NC
     * plane. The same-node CC plane must stay cacheable to preserve its
     * measured low latency. */
    open_flags = O_RDWR | O_CLOEXEC;
    if (info->plane == UCT_OBMM_PLANE_NC) {
        open_flags |= O_SYNC;
    }

    fd = open(info->dev_path, open_flags);
    if (fd < 0) {
        ucs_debug("obmm: open(%s) failed: %m", info->dev_path);
        return UCS_ERR_IO_ERROR;
    }

    map = mmap(NULL, info->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        ucs_debug("obmm: mmap(%s, size=0x%" PRIx64 ") failed: %m",
                  info->dev_path, info->size);
        status = UCS_ERR_IO_ERROR;
        goto err_close;
    }

    region->info   = *info;
    region->fd     = fd;
    region->base   = map;
    region->length = info->size;

    ucs_debug("obmm: mapped %s memid=%" PRIu64 " size=0x%" PRIx64
              " base=%p type=%s plane=%s dcna=0x%" PRIx64
              " region_id=0x%x",
              info->dev_path, info->memid, info->size, map,
              (info->type == UCT_OBMM_DEV_EXPORT) ? "export" : "import",
              (info->plane == UCT_OBMM_PLANE_CC) ? "cc" : "nc",
              info->exporter_dcna, info->region_id);

    return UCS_OK;

err_close:
    close(fd);
    return status;
}


void uct_obmm_region_close(uct_obmm_region_t *region)
{
    if (region->base != NULL) {
        if (munmap(region->base, region->length) != 0) {
            ucs_warn("obmm: munmap(%s, %p, 0x%zx) failed: %m",
                     region->info.dev_path, region->base, region->length);
        }
        region->base = NULL;
    }

    if (region->fd >= 0) {
        close(region->fd);
        region->fd = -1;
    }
}
