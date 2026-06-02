/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_REGION_H_
#define UCT_OBMM_REGION_H_

#include "obmm_sysfs.h"

#include <ucs/type/status.h>


/**
 * A live mapping of one obmm shmdev region. The fd is kept open for the
 * lifetime of the mapping (kernel may require it for syncing / liveness).
 *
 * NC regions are mapped with O_SYNC + MAP_SHARED so writes from this host
 * are non-cacheable and visible to remote hosts without
 * obmm_set_ownership() flips.  CC regions are mapped without O_SYNC and
 * start at PROT_NONE; callers use obmm_set_ownership() to acquire access.
 * See `.github/skills/obmm-api-and-env/SKILL.md` for rationale.
 */
typedef struct uct_obmm_region {
    uct_obmm_dev_info_t info;       /* sysfs-derived metadata        */
    int                 fd;         /* open(dev_path, O_RDWR|O_SYNC) */
    int                 is_cc;      /* non-zero if CC (no O_SYNC)    */
    void               *base;       /* mmap base, length = info.size */
    size_t              length;
} uct_obmm_region_t;


/**
 * Open + mmap an NC region described by `info`. On success the caller owns
 * the mapping and must release it via uct_obmm_region_close().
 */
ucs_status_t uct_obmm_region_open(const uct_obmm_dev_info_t *info,
                                  uct_obmm_region_t *region);

/**
 * Open + mmap a CC (cache-coherent) region.  Differs from the NC path:
 *   - open(fd, O_RDWR)  -- NO O_SYNC, mapping is cacheable.
 *   - mmap(PROT_NONE)   -- start with no access; caller must use
 *     obmm_set_ownership() to acquire PROT_READ or PROT_WRITE before
 *     accessing the mapping.
 */
ucs_status_t uct_obmm_region_open_cc(const uct_obmm_dev_info_t *info,
                                     uct_obmm_region_t *region);

void uct_obmm_region_close(uct_obmm_region_t *region);


/* ---- obmm_set_ownership dlopen wrapper (CC transport support) ---- */

typedef int (*uct_obmm_set_ownership_fn_t)(int fd, void *start, void *end,
                                           int prot);

uct_obmm_set_ownership_fn_t uct_obmm_cc_get_set_ownership(void);

static inline int
uct_obmm_cc_set_ownership(int fd, void *start, void *end, int prot)
{
    uct_obmm_set_ownership_fn_t fn = uct_obmm_cc_get_set_ownership();
    if (fn == NULL) {
        return -1;
    }
    return fn(fd, start, end, prot);
}


#endif
