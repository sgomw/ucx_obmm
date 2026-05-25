/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "obmm_ownership.h"

#include <ucs/debug/log.h>
#include <ucs/sys/ptr_arith.h>

#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>


typedef int (*uct_obmm_set_ownership_func_t)(int fd, void *start, void *end,
                                             int prot);


static uct_obmm_set_ownership_func_t
uct_obmm_resolve_set_ownership(void)
{
    static uct_obmm_set_ownership_func_t func;
    static int                           resolved;
    static void                         *handles[3];
    const char                          *env_path;
    const char                          *candidates[2];
    int                                  i;

    if (resolved) {
        return func;
    }

    resolved = 1;
    func     = (uct_obmm_set_ownership_func_t)dlsym(RTLD_DEFAULT,
                                                    "obmm_set_ownership");
    if (func != NULL) {
        return func;
    }

    env_path      = getenv("OBMM_LIBOBMM_PATH");
    candidates[0] = "libobmm.so";
    candidates[1] = "libobmm.so.0";

    if ((env_path != NULL) && (*env_path != '\0')) {
        handles[0] = dlopen(env_path, RTLD_LAZY | RTLD_LOCAL);
        if (handles[0] != NULL) {
            func = (uct_obmm_set_ownership_func_t)dlsym(handles[0],
                                                        "obmm_set_ownership");
            if (func != NULL) {
                return func;
            }
        }
    }

    for (i = 0; i < 2; ++i) {
        handles[i + 1] = dlopen(candidates[i], RTLD_LAZY | RTLD_LOCAL);
        if (handles[i + 1] == NULL) {
            continue;
        }

        func = (uct_obmm_set_ownership_func_t)dlsym(handles[i + 1],
                                                    "obmm_set_ownership");
        if (func != NULL) {
            return func;
        }
    }

    return NULL;
}


ucs_status_t uct_obmm_region_set_ownership(uct_obmm_region_t *region, void *start,
                                           size_t length, int prot)
{
    uct_obmm_set_ownership_func_t func;
    void                         *end;

    if (region->map_mode != UCT_OBMM_MAP_MODE_CC) {
        ucs_error("obmm: obmm_set_ownership() is only valid on CC mappings "
                  "(memid=%lu mode=%d)",
                  (unsigned long)region->info.memid, region->map_mode);
        return UCS_ERR_INVALID_PARAM;
    }

    if (length == 0) {
        ucs_error("obmm: obmm_set_ownership() length must be > 0");
        return UCS_ERR_INVALID_PARAM;
    }

    func = uct_obmm_resolve_set_ownership();
    if (func == NULL) {
        ucs_error("obmm: failed to resolve obmm_set_ownership; set "
                  "OBMM_LIBOBMM_PATH or place libobmm.so in the loader path");
        return UCS_ERR_UNSUPPORTED;
    }

    end = UCS_PTR_BYTE_OFFSET(start, length);
    if (func(region->fd, start, end, prot) != 0) {
        ucs_error("obmm: obmm_set_ownership(memid=%lu start=%p end=%p prot=%d) "
                  "failed: %s",
                  (unsigned long)region->info.memid, start, end, prot,
                  strerror(errno));
        return UCS_ERR_IO_ERROR;
    }

    return UCS_OK;
}
