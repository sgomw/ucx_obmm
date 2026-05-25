/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_BULK_H_
#define UCT_OBMM_BULK_H_

#include "obmm_fifo.h"
#include "obmm_pool.h"

#include <ucs/sys/compiler.h>
#include <ucs/sys/compiler_def.h>
#include <ucs/sys/ptr_arith.h>
#include <ucs/type/status.h>

#include <stdint.h>


#define UCT_OBMM_CC_LOCAL_FIFO_SIZE     1u
#define UCT_OBMM_CC_LOCAL_FIFO_ELEM_SIZE UCS_SYS_CACHE_LINE_SIZE
#define UCT_OBMM_CC_LOCAL_BCOPY_SEG_SIZE 0u
#define UCT_OBMM_CC_BULK_WINDOW_ALIGN   (2ul * 1024ul * 1024ul)
#define UCT_OBMM_BULK_CTRL_MAGIC        0x4f424d42u /* "OBMB" */
#define UCT_OBMM_BULK_CTRL_VERSION      1u


enum {
    UCT_OBMM_BULK_DESC_FLAG_REMOTE_OWNERSHIP = UCS_BIT(0)
};


typedef struct uct_obmm_bulk_ctrl_hdr {
    volatile uint32_t magic;
    volatile uint32_t generation;
    volatile uint64_t cc_memid;
    volatile uint64_t window_size;
    volatile uint32_t window_count;
    volatile uint32_t version;
    volatile uint64_t req_seq;
} UCS_V_ALIGNED(UCS_SYS_CACHE_LINE_SIZE) uct_obmm_bulk_ctrl_hdr_t;


typedef struct uct_obmm_bulk_window_desc {
    volatile uint64_t seq;
    volatile uint64_t ack_seq;
    uint64_t          cc_memid;
    uint32_t          length;
    uint32_t          target_slot_index;
    uint32_t          target_generation;
    uint8_t           am_id;
    uint8_t           flags;
    uint16_t          reserved2;
    uint32_t          sender_generation;
    uint32_t          ack_generation;
    UCS_CACHELINE_PADDING(uint64_t);
} UCS_V_ALIGNED(UCS_SYS_CACHE_LINE_SIZE) uct_obmm_bulk_window_desc_t;


static UCS_F_ALWAYS_INLINE size_t
uct_obmm_bulk_ctrl_size(unsigned window_count)
{
    return ucs_align_up(sizeof(uct_obmm_bulk_ctrl_hdr_t) +
                        ((size_t)window_count *
                         sizeof(uct_obmm_bulk_window_desc_t)),
                        UCS_SYS_CACHE_LINE_SIZE);
}


static UCS_F_ALWAYS_INLINE uct_obmm_bulk_ctrl_hdr_t *
uct_obmm_bulk_ctrl_hdr(void *slot_base)
{
    return (uct_obmm_bulk_ctrl_hdr_t*)slot_base;
}


static UCS_F_ALWAYS_INLINE uct_obmm_bulk_window_desc_t *
uct_obmm_bulk_ctrl_descs(void *slot_base)
{
    return (uct_obmm_bulk_window_desc_t*)
           UCS_PTR_BYTE_OFFSET(slot_base, sizeof(uct_obmm_bulk_ctrl_hdr_t));
}


static UCS_F_ALWAYS_INLINE uct_obmm_bulk_window_desc_t *
uct_obmm_bulk_ctrl_desc(void *slot_base, unsigned index)
{
    return &uct_obmm_bulk_ctrl_descs(slot_base)[index];
}


static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_cc_local_layout(size_t *slot_stride_p, size_t *pool_size_p,
                         size_t *prefix_size_p)
{
    size_t slot_stride = uct_obmm_slot_stride(UCT_OBMM_CC_LOCAL_FIFO_SIZE,
                                             UCT_OBMM_CC_LOCAL_FIFO_ELEM_SIZE,
                                             UCT_OBMM_CC_LOCAL_BCOPY_SEG_SIZE);
    size_t pool_size;

    if (slot_stride > UINT32_MAX) {
        return UCS_ERR_INVALID_PARAM;
    }

    pool_size = uct_obmm_pool_required_size(UCT_OBMM_POOL_SLOT_COUNT,
                                           (uint32_t)slot_stride);
    if (slot_stride_p != NULL) {
        *slot_stride_p = slot_stride;
    }
    if (pool_size_p != NULL) {
        *pool_size_p = pool_size;
    }
    if (prefix_size_p != NULL) {
        *prefix_size_p = ucs_align_up(pool_size, UCT_OBMM_CC_BULK_WINDOW_ALIGN);
    }
    return UCS_OK;
}


static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_cc_local_pool_region(void *region_base, size_t region_length,
                              void **pool_base_p, size_t *pool_length_p)
{
    size_t prefix_size;
    ucs_status_t status;

    status = uct_obmm_cc_local_layout(NULL, NULL, &prefix_size);
    if (status != UCS_OK) {
        return status;
    }
    if (region_length < prefix_size) {
        return UCS_ERR_NO_RESOURCE;
    }

    *pool_base_p   = region_base;
    *pool_length_p = prefix_size;
    return UCS_OK;
}


static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_cc_bulk_data_region_split(void *region_base, size_t region_length,
                                   void **data_base_p, size_t *data_offset_p,
                                   size_t *data_length_p)
{
    size_t prefix_size;
    ucs_status_t status;

    status = uct_obmm_cc_local_layout(NULL, NULL, &prefix_size);
    if (status != UCS_OK) {
        return status;
    }
    if (region_length <= prefix_size) {
        return UCS_ERR_NO_RESOURCE;
    }

    *data_offset_p = prefix_size;
    *data_base_p   = UCS_PTR_BYTE_OFFSET(region_base, *data_offset_p);
    *data_length_p = region_length - *data_offset_p;
    return UCS_OK;
}


#endif
