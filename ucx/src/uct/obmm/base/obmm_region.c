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
#include <ucs/sys/math.h>
#include <ucs/sys/sys.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>


typedef int (*uct_obmm_set_ownership_func_t)(int fd, void *start, void *end,
                                             int prot);

#define UCT_OBMM_REGION_CC_MAP_ALIGNMENT (2ul * 1024ul * 1024ul)


static const char *
uct_obmm_region_kind_name(uct_obmm_region_kind_t kind)
{
    return (kind == UCT_OBMM_REGION_KIND_CC) ? "cc" : "nc";
}


static uct_obmm_set_ownership_func_t uct_obmm_region_get_ownership_func(void)
{
    static uct_obmm_set_ownership_func_t fn       = NULL;
    static int                           resolved = 0;
    void                                *handle;
    const char                          *dlerr;

    if (resolved) {
        return fn;
    }

    resolved = 1;
    handle   = dlopen("libobmm.so", RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        handle = dlopen("libobmm.so.0", RTLD_NOW | RTLD_LOCAL);
    }
    if (handle == NULL) {
        dlerr = dlerror();
        ucs_debug("obmm: failed to load libobmm for ownership: %s",
                  dlerr != NULL ? dlerr : "unknown error");
        return NULL;
    }

    fn = (uct_obmm_set_ownership_func_t)dlsym(handle, "obmm_set_ownership");
    if (fn == NULL) {
        dlerr = dlerror();
        ucs_debug("obmm: libobmm does not provide obmm_set_ownership: %s",
                  dlerr != NULL ? dlerr : "unknown error");
    }
    return fn;
}


static void*
uct_obmm_region_mmap_aligned(size_t length, int prot, int flags, int fd,
                             off_t offset, size_t alignment)
{
    size_t    page_size = ucs_get_page_size();
    size_t    reserve_length;
    void     *reserve;
    void     *map;
    uintptr_t reserve_addr;
    uintptr_t aligned_addr;
    size_t    prefix;
    size_t    suffix;
    int       saved_errno;

    if (alignment <= page_size) {
        return mmap(NULL, length, prot, flags, fd, offset);
    }

    if (length > (SIZE_MAX - alignment)) {
        errno = ENOMEM;
        return MAP_FAILED;
    }

    reserve_length = length + alignment;
    reserve = mmap(NULL, reserve_length, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reserve == MAP_FAILED) {
        return MAP_FAILED;
    }

    reserve_addr = (uintptr_t)reserve;
    aligned_addr = ucs_align_up(reserve_addr, alignment);
    prefix       = aligned_addr - reserve_addr;
    suffix       = reserve_length - prefix - length;

    if ((prefix > 0) && (munmap(reserve, prefix) != 0)) {
        saved_errno = errno;
        munmap(reserve, reserve_length);
        errno = saved_errno;
        return MAP_FAILED;
    }

    if ((suffix > 0) &&
        (munmap((void*)(aligned_addr + length), suffix) != 0)) {
        saved_errno = errno;
        munmap((void*)aligned_addr, length);
        errno = saved_errno;
        return MAP_FAILED;
    }

    map = mmap((void*)aligned_addr, length, prot, flags | MAP_FIXED, fd,
               offset);
    if (map == MAP_FAILED) {
        saved_errno = errno;
        munmap((void*)aligned_addr, length);
        errno = saved_errno;
        return MAP_FAILED;
    }

    return map;
}


ucs_status_t uct_obmm_region_open(const uct_obmm_dev_info_t *info,
                                  uct_obmm_region_t *region)
{
    ucs_status_t status;
    void        *map;
    int          open_flags;
    int          mmap_prot;
    int          fd;

    if (!info->allow_mmap) {
        ucs_debug("obmm: device %s does not support mmap", info->dev_path);
        return UCS_ERR_UNSUPPORTED;
    }

    if (info->size == 0) {
        ucs_debug("obmm: device %s reports zero size", info->dev_path);
        return UCS_ERR_NO_RESOURCE;
    }

    open_flags = O_RDWR | O_CLOEXEC;
    if (info->kind == UCT_OBMM_REGION_KIND_NC) {
        /* O_SYNC selects the non-cacheable mapping, which is required for
         * cross-host shared FIFO/control use without ownership flips. */
        open_flags |= O_SYNC;
    }

    fd = open(info->dev_path, open_flags);
    if (fd < 0) {
        ucs_debug("obmm: open(%s) failed: %m", info->dev_path);
        return UCS_ERR_IO_ERROR;
    }

    mmap_prot = (info->kind == UCT_OBMM_REGION_KIND_CC) ?
                PROT_NONE : (PROT_READ | PROT_WRITE);

    if (info->kind == UCT_OBMM_REGION_KIND_CC) {
        map = uct_obmm_region_mmap_aligned(info->size, mmap_prot,
                                           MAP_SHARED, fd, 0,
                                           UCT_OBMM_REGION_CC_MAP_ALIGNMENT);
    } else {
        map = mmap(NULL, info->size, mmap_prot, MAP_SHARED, fd, 0);
    }
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
              " base=%p type=%s kind=%s dcna=0x%" PRIx64,
              info->dev_path, info->memid, info->size, map,
              (info->type == UCT_OBMM_DEV_EXPORT) ? "export" : "import",
              uct_obmm_region_kind_name(info->kind),
              info->exporter_dcna);

    return UCS_OK;

err_close:
    close(fd);
    return status;
}


ucs_status_t uct_obmm_region_set_ownership(uct_obmm_region_t *region,
                                           void *start, size_t length,
                                           int prot)
{
    uct_obmm_set_ownership_func_t fn;
    uintptr_t                     start_addr = (uintptr_t)start;
    uintptr_t                     base_addr  = (uintptr_t)region->base;
    size_t                        page_size  = ucs_get_page_size();
    size_t                        offset;

    if (region->info.kind != UCT_OBMM_REGION_KIND_CC) {
        ucs_error("obmm: ownership requested on non-CC region memid=%" PRIu64,
                  region->info.memid);
        return UCS_ERR_INVALID_PARAM;
    }

    if ((length == 0) || ((start_addr % page_size) != 0) ||
        ((length % page_size) != 0) || (start_addr < base_addr)) {
        ucs_error("obmm: invalid ownership range memid=%" PRIu64
                  " start=%p length=%zu page=%zu base=%p region=%zu",
                  region->info.memid, start, length, page_size, region->base,
                  region->length);
        return UCS_ERR_INVALID_PARAM;
    }

    offset = start_addr - base_addr;
    if ((offset > region->length) || (length > (region->length - offset))) {
        ucs_error("obmm: ownership range exceeds region memid=%" PRIu64
                  " start=%p length=%zu base=%p region=%zu",
                  region->info.memid, start, length, region->base,
                  region->length);
        return UCS_ERR_INVALID_PARAM;
    }

    fn = uct_obmm_region_get_ownership_func();
    if (fn == NULL) {
        return UCS_ERR_UNSUPPORTED;
    }

    if (fn(region->fd, start, (char*)start + length, prot) != 0) {
        ucs_debug("obmm: obmm_set_ownership(memid=%" PRIu64
                  ", start=%p, length=%zu, prot=0x%x) failed: %m",
                  region->info.memid, start, length, prot);
        return (errno == EBUSY) ? UCS_ERR_NO_RESOURCE : UCS_ERR_IO_ERROR;
    }

    return UCS_OK;
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
