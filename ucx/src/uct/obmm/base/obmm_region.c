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
#include <ucs/sys/compiler.h>

#include <fcntl.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <dlfcn.h>


ucs_status_t uct_obmm_region_open(const uct_obmm_dev_info_t *info,
                                  uct_obmm_region_t *region)
{
    ucs_status_t status;
    void        *map;
    int          fd;

    if (!info->allow_mmap) {
        ucs_debug("obmm: device %s does not support mmap", info->dev_path);
        return UCS_ERR_UNSUPPORTED;
    }

    if (info->size == 0) {
        ucs_debug("obmm: device %s reports zero size", info->dev_path);
        return UCS_ERR_NO_RESOURCE;
    }

    /* O_SYNC selects the non-cacheable mapping, which is required for
     * cross-host shared FIFO use without obmm_set_ownership() flips. */
    fd = open(info->dev_path, O_RDWR | O_SYNC | O_CLOEXEC);
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
    region->is_cc  = 0;
    region->base   = map;
    region->length = info->size;

    ucs_debug("obmm: mapped %s memid=%" PRIu64 " size=0x%" PRIx64
              " base=%p type=%s dcna=0x%" PRIx64,
              info->dev_path, info->memid, info->size, map,
              (info->type == UCT_OBMM_DEV_EXPORT) ? "export" : "import",
              info->exporter_dcna);

    return UCS_OK;

err_close:
    close(fd);
    return status;
}


ucs_status_t uct_obmm_region_open_cc(const uct_obmm_dev_info_t *info,
                                     uct_obmm_region_t *region)
{
    ucs_status_t status;
    void        *map;
    int          fd;

    if (!info->allow_mmap) {
        ucs_debug("obmm: CC device %s does not support mmap",
                  info->dev_path);
        return UCS_ERR_UNSUPPORTED;
    }

    if (info->size == 0) {
        ucs_debug("obmm: CC device %s reports zero size",
                  info->dev_path);
        return UCS_ERR_NO_RESOURCE;
    }

    /* No O_SYNC: cacheable mapping.  mmap with PROT_NONE so the
     * kernel does not grant implicit read/write on a region that
     * requires explicit obmm_set_ownership() calls for every access. */
    fd = open(info->dev_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ucs_debug("obmm: CC open(%s) failed: %m", info->dev_path);
        return UCS_ERR_IO_ERROR;
    }

    map = mmap(NULL, info->size, PROT_NONE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        ucs_debug("obmm: CC mmap(%s, size=0x%" PRIx64 ") failed: %m",
                  info->dev_path, info->size);
        status = UCS_ERR_IO_ERROR;
        goto err_close;
    }

    region->info   = *info;
    region->fd     = fd;
    region->is_cc  = 1;
    region->base   = map;
    region->length = info->size;

    ucs_debug("obmm: CC mapped %s memid=%" PRIu64
              " size=0x%" PRIx64 " base=%p type=%s dcna=0x%" PRIx64,
              info->dev_path, info->memid, info->size, map,
              (info->type == UCT_OBMM_DEV_EXPORT) ? "export"
                                                    : "import",
              info->exporter_dcna);

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

/* ---- obmm_set_ownership dlopen resolver ---- */

static uct_obmm_set_ownership_fn_t uct_obmm_cc_fn   = NULL;
static volatile int                uct_obmm_cc_tried = 0;

uct_obmm_set_ownership_fn_t uct_obmm_cc_get_set_ownership(void)
{
    void *handle;

    if (ucs_unlikely(!uct_obmm_cc_tried)) {
        handle = dlopen("libobmm.so", RTLD_NOW | RTLD_GLOBAL);
        if (handle == NULL) {
            ucs_debug("obmm: dlopen(libobmm.so) failed: %s", dlerror());
            uct_obmm_cc_fn = NULL;
        } else {
            uct_obmm_cc_fn = (uct_obmm_set_ownership_fn_t)
                             dlsym(handle, "obmm_set_ownership");
            if (uct_obmm_cc_fn == NULL) {
                ucs_error("obmm: dlsym(obmm_set_ownership) failed: %s",
                          dlerror());
            }
        }
        ucs_compiler_fence();
        uct_obmm_cc_tried = 1;
    }
    return uct_obmm_cc_fn;
}
