/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_BLOCK_H_
#define UCT_OBMM_BLOCK_H_

#include "obmm_fifo.h"

#include <ucs/sys/compiler_def.h>
#include <ucs/type/status.h>

#include <stddef.h>
#include <stdint.h>


#define UCT_OBMM_BLOCK_COLOR_ID_NONE UINT32_MAX


/* Header at region base. One shmdev block contains exactly one FIFO. */
typedef struct uct_obmm_block_hdr {
    uint64_t claim;             /* 0=free, token=initing, token|READY=ready */
    uint64_t fifo_offset;       /* offset from region base */
} uct_obmm_block_hdr_t;


/* Cached pointers into one mapped block. */
typedef struct uct_obmm_block {
    void                  *base;
    size_t                 length;
    uct_obmm_block_hdr_t  *hdr;
    void                  *fifo;
    uct_obmm_fifo_ctl_t   *ctl;
    void                  *elems;
    size_t                 fifo_offset;
    size_t                 fifo_stride;
} uct_obmm_block_t;


size_t uct_obmm_block_min_fifo_offset(void);

size_t uct_obmm_block_colored_fifo_offset(size_t fifo_stride,
                                          size_t region_size,
                                          uint32_t region_id);

size_t uct_obmm_block_required_size(size_t fifo_stride,
                                    size_t fifo_offset);

ucs_status_t uct_obmm_block_attach(void *region_base, size_t region_size,
                                   size_t fifo_stride,
                                   size_t fifo_offset,
                                   uct_obmm_block_t *block);

ucs_status_t uct_obmm_block_open(void *region_base, size_t region_size,
                                 size_t fifo_stride,
                                 uct_obmm_block_t *block);

void uct_obmm_block_release(uct_obmm_block_t *block);

#endif
