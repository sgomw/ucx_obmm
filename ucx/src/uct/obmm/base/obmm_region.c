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
#include <ucs/sys/sys.h>
#include <ucs/sys/math.h>
#include <ucs/sys/ptr_arith.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>


typedef int (*uct_obmm_set_ownership_func_t)(int fd, void *start, void *end,
                                             int prot);

static uct_obmm_set_ownership_func_t uct_obmm_set_ownership_func = NULL;
static int                           uct_obmm_set_ownership_resolved = 0;


static ucs_status_t uct_obmm_resolve_set_ownership(void)
{
    void *handle;

    if (uct_obmm_set_ownership_resolved) {
        return (uct_obmm_set_ownership_func == NULL) ? UCS_ERR_UNSUPPORTED :
                                                       UCS_OK;
    }

    handle = dlopen("libobmm.so", RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        handle = dlopen("libobmm.so.0", RTLD_NOW | RTLD_LOCAL);
    }
    if (handle == NULL) {
        ucs_error("obmm: failed to load libobmm for cacheable ownership: %s",
                  dlerror());
        uct_obmm_set_ownership_resolved = 1;
        return UCS_ERR_UNSUPPORTED;
    }

    uct_obmm_set_ownership_func =
        (uct_obmm_set_ownership_func_t)dlsym(handle, "obmm_set_ownership");
    if (uct_obmm_set_ownership_func == NULL) {
        ucs_error("obmm: libobmm does not export obmm_set_ownership: %s",
                  dlerror());
        uct_obmm_set_ownership_resolved = 1;
        return UCS_ERR_UNSUPPORTED;
    }

    uct_obmm_set_ownership_resolved = 1;
    return UCS_OK;
}


ucs_status_t uct_obmm_region_open(const uct_obmm_dev_info_t *info,
                                  uct_obmm_region_mode_t mode,
                                  uct_obmm_region_t *region)
{
    ucs_status_t status;
    void        *map;
    int          fd;
    int          open_flags;
    int          prot;

    if (!info->allow_mmap) {
        ucs_error("obmm: device %s does not support mmap", info->dev_path);
        return UCS_ERR_UNSUPPORTED;
    }

    if (info->size == 0) {
        ucs_error("obmm: device %s reports zero size", info->dev_path);
        return UCS_ERR_NO_RESOURCE;
    }

    if ((mode != UCT_OBMM_REGION_NC) && (mode != UCT_OBMM_REGION_CC)) {
        return UCS_ERR_INVALID_PARAM;
    }

    open_flags = O_CLOEXEC;
    prot       = PROT_NONE;
    if (mode == UCT_OBMM_REGION_NC) {
        /* O_SYNC selects the non-cacheable mapping, which is required for
         * cross-host shared FIFO use without obmm_set_ownership() flips. */
        open_flags |= O_RDWR | O_SYNC;
        prot        = PROT_READ | PROT_WRITE;
    } else if (info->type == UCT_OBMM_DEV_EXPORT) {
        /* Local CC exports are used as TX chunk storage and need write
         * ownership. Peer CC imports are RX-only and may be read-only devices. */
        open_flags |= O_RDWR;
    } else {
        open_flags |= O_RDONLY;
    }

    fd = open(info->dev_path, open_flags);
    if (fd < 0) {
        ucs_error("obmm: open(%s) failed: %m", info->dev_path);
        return UCS_ERR_IO_ERROR;
    }

    map = mmap(NULL, info->size, prot, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        ucs_error("obmm: mmap(%s, size=0x%" PRIx64 ", mode=%s) failed: %m",
                  info->dev_path, info->size,
                  (mode == UCT_OBMM_REGION_NC) ? "nc" : "cc");
        status = UCS_ERR_IO_ERROR;
        goto err_close;
    }

    region->info   = *info;
    region->fd     = fd;
    region->base   = map;
    region->length = info->size;
    region->mode   = mode;

    ucs_debug("obmm: mapped %s memid=%" PRIu64 " size=0x%" PRIx64
              " base=%p type=%s dcna=0x%" PRIx64 " mode=%s",
              info->dev_path, info->memid, info->size, map,
              (info->type == UCT_OBMM_DEV_EXPORT) ? "export" : "import",
              info->exporter_dcna,
              (mode == UCT_OBMM_REGION_NC) ? "nc" : "cc");

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


ucs_status_t uct_obmm_region_set_ownership(uct_obmm_region_t *region,
                                           void *start, size_t length,
                                           int prot)
{
    uintptr_t    start_addr = (uintptr_t)start;
    uintptr_t    base       = (uintptr_t)region->base;
    size_t       page_size  = ucs_get_page_size();
    void        *end;
    ucs_status_t status;

    if (region->mode != UCT_OBMM_REGION_CC) {
        ucs_error("obmm: set_ownership requested on non-cacheable region "
                  "memid=%" PRIu64, region->info.memid);
        return UCS_ERR_INVALID_PARAM;
    }

    if ((length == 0) || (start_addr < base) ||
        ((start_addr + length) > (base + region->length)) ||
        ((start_addr & (page_size - 1)) != 0) ||
        ((length & (page_size - 1)) != 0)) {
        ucs_error("obmm: invalid ownership range memid=%" PRIu64
                  " start=%p length=%zu base=%p region=%zu page=%zu",
                  region->info.memid, start, length, region->base,
                  region->length, page_size);
        return UCS_ERR_INVALID_PARAM;
    }

    status = uct_obmm_resolve_set_ownership();
    if (status != UCS_OK) {
        return status;
    }

    end = UCS_PTR_BYTE_OFFSET(start, length);
    if (uct_obmm_set_ownership_func(region->fd, start, end, prot) == 0) {
        return UCS_OK;
    }

    if (errno == ENOTRECOVERABLE) {
        ucs_fatal("obmm: set_ownership(%s, %p..%p, prot=0x%x) failed with "
                  "ENOTRECOVERABLE", region->info.dev_path, start, end, prot);
    }

    ucs_error("obmm: set_ownership(%s, %p..%p, prot=0x%x) failed: %m",
              region->info.dev_path, start, end, prot);
    if (errno == EBUSY) {
        return UCS_ERR_NO_RESOURCE;
    } else if (errno == EINVAL) {
        return UCS_ERR_INVALID_PARAM;
    }
    return UCS_ERR_IO_ERROR;
}


ucs_status_t uct_obmm_region_zero(uct_obmm_region_t *region)
{
    ucs_status_t status;

    if ((region == NULL) || (region->base == NULL)) {
        return UCS_OK;
    }

    if (region->info.type != UCT_OBMM_DEV_EXPORT) {
        ucs_error("obmm: zero requested on non-export region memid=%" PRIu64,
                  region->info.memid);
        return UCS_ERR_INVALID_PARAM;
    }

    if (region->mode == UCT_OBMM_REGION_CC) {
        status = uct_obmm_region_set_ownership(region, region->base,
                                               region->length, PROT_WRITE);
        if (status != UCS_OK) {
            return status;
        }
    }

    memset(region->base, 0, region->length);
    ucs_memory_bus_store_fence();

    if (region->mode == UCT_OBMM_REGION_CC) {
        status = uct_obmm_region_set_ownership(region, region->base,
                                               region->length, PROT_NONE);
        if (status != UCS_OK) {
            return status;
        }
    }

    return UCS_OK;
}
