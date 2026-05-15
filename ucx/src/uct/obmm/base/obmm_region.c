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

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef int (*uct_obmm_set_ownership_func_t)(int fd, void *start, void *end,
                                             int prot);


int uct_obmm_diag_cc_handoff_enabled(void)
{
    static int cached = -1;
    const char *env;

    if (cached >= 0) {
        return cached;
    }

    env = getenv("UCX_OBMM_DIAG_CC_HANDOFF");
    cached = (env != NULL) && (env[0] != '\0') &&
             ((env[0] != '0') || (env[1] != '\0'));
    return cached;
}


static uct_obmm_set_ownership_func_t
uct_obmm_region_resolve_set_ownership(void)
{
    static uct_obmm_set_ownership_func_t func;
    static void                         *handle;
    static int                           resolved;
    const char                          *error;

    if (!resolved) {
        handle = dlopen("libobmm.so", RTLD_NOW | RTLD_LOCAL);
        if (handle == NULL) {
            handle = dlopen("libobmm.so.0", RTLD_NOW | RTLD_LOCAL);
        }
        if (handle == NULL) {
            error = dlerror();
            ucs_error("obmm: failed to load libobmm for ownership handoff: %s",
                      (error == NULL) ? "unknown error" : error);
        } else {
            func = (uct_obmm_set_ownership_func_t)dlsym(handle,
                                                        "obmm_set_ownership");
            if (func == NULL) {
                error = dlerror();
                ucs_error("obmm: libobmm does not export "
                          "obmm_set_ownership: %s",
                          (error == NULL) ? "unknown error" : error);
            }
        }
        resolved = 1;
    }

    return func;
}


ucs_status_t uct_obmm_region_set_ownership(uct_obmm_region_t *region,
                                           void *start, size_t length,
                                           int prot)
{
    uct_obmm_set_ownership_func_t func;
    uintptr_t                     base;
    uintptr_t                     begin;
    uintptr_t                     end;
    size_t                        page_size;
    int                           ret;
    int                           saved_errno;

    if (region->mode != UCT_OBMM_REGION_CC) {
        ucs_error("obmm: set_ownership is valid only on CC regions");
        return UCS_ERR_INVALID_PARAM;
    }

    if (length == 0) {
        ucs_error("obmm: set_ownership requires non-zero length");
        return UCS_ERR_INVALID_PARAM;
    }

    if ((prot != PROT_NONE) && (prot != PROT_READ) &&
        (prot != PROT_WRITE) && (prot != (PROT_READ | PROT_WRITE))) {
        ucs_error("obmm: invalid set_ownership prot=%d", prot);
        return UCS_ERR_INVALID_PARAM;
    }

    base      = (uintptr_t)region->base;
    begin     = (uintptr_t)start;
    end       = begin + length;
    page_size = ucs_get_page_size();
    if ((begin < base) || (end > (base + region->length)) ||
        ((begin % page_size) != 0) || ((end % page_size) != 0)) {
        ucs_error("obmm: set_ownership range [%p,%p) invalid for %s "
                  "(base=%p len=%zu page=%zu)", start, (void*)end,
                  region->info.dev_path, region->base, region->length,
                  page_size);
        return UCS_ERR_INVALID_PARAM;
    }

    func = uct_obmm_region_resolve_set_ownership();
    if (func == NULL) {
        return UCS_ERR_UNSUPPORTED;
    }

    errno = 0;
    ret   = func(region->fd, start, (void*)end, prot);
    if (ret == 0) {
        return UCS_OK;
    }

    saved_errno = errno;
    ucs_error("obmm: obmm_set_ownership(%s, [%p,%p), prot=%d) failed: %s",
              region->info.dev_path, start, (void*)end, prot,
              strerror(saved_errno));
    switch (saved_errno) {
    case EBUSY:
        return UCS_ERR_NO_RESOURCE;
    case EINVAL:
        return UCS_ERR_INVALID_PARAM;
    default:
        return UCS_ERR_IO_ERROR;
    }
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
    prot       = 0;
    if (mode == UCT_OBMM_REGION_NC) {
        /* O_SYNC selects the non-cacheable mapping, which is required for
         * cross-host shared FIFO use without permission flips. */
        open_flags |= O_RDWR | O_SYNC;
        prot        = PROT_READ | PROT_WRITE;
    } else if (info->type == UCT_OBMM_DEV_EXPORT) {
        open_flags |= O_RDWR;
        prot        = uct_obmm_diag_cc_handoff_enabled() ?
                      PROT_NONE : (PROT_READ | PROT_WRITE);
    } else {
        open_flags |= O_RDONLY;
        prot        = uct_obmm_diag_cc_handoff_enabled() ? PROT_NONE :
                                                          PROT_READ;
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
