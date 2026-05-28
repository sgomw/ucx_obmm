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


#define UCT_OBMM_DEFAULT_BCOPY_SEG_SIZE      32768u
#define UCT_OBMM_DEFAULT_BCOPY_SEG_SIZE_STR  "32768"
#define UCT_OBMM_CC_REPORTED_MAX_BCOPY       57344u
#define UCT_OBMM_CC_LOCAL_FIFO_SIZE          8u
#define UCT_OBMM_CC_LOCAL_FIFO_ELEM_SIZE     UCS_SYS_CACHE_LINE_SIZE
#define UCT_OBMM_CC_LOCAL_BCOPY_SEG_SIZE     65600u
#define UCT_OBMM_CC_LOCAL_DESC_PREFIX        (2u * UCS_SYS_CACHE_LINE_SIZE)
#define UCT_OBMM_CC_LOCAL_DESC_COUNT         (2u * UCT_OBMM_CC_LOCAL_FIFO_SIZE)
#define UCT_OBMM_CC_BULK_WINDOW_ALIGN        (2ul * 1024ul * 1024ul)
#define UCT_OBMM_BULK_CTRL_MAGIC             0x4f424d42u /* "OBMB" */
#define UCT_OBMM_BULK_CTRL_VERSION           1u


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


typedef struct uct_obmm_cc_recv_desc_meta {
    void    *path;
    uint32_t desc_index;
    uint32_t reserved;
} UCS_S_PACKED uct_obmm_cc_recv_desc_meta_t;


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


static UCS_F_ALWAYS_INLINE size_t
uct_obmm_nc_bulk_ctrl_offset(uint32_t slot_count, uint32_t slot_size)
{
    return ucs_align_up(uct_obmm_pool_required_size(slot_count, slot_size),
                        UCS_SYS_CACHE_LINE_SIZE);
}


static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_nc_bulk_ctrl_region(void *region_base, size_t region_length,
                             uint32_t slot_count, uint32_t slot_size,
                             unsigned window_count, void **base_p,
                             size_t *offset_p, size_t *stride_p,
                             size_t *length_p)
{
    size_t offset = uct_obmm_nc_bulk_ctrl_offset(slot_count, slot_size);
    size_t stride = uct_obmm_bulk_ctrl_size(window_count);
    size_t length = (size_t)slot_count * stride;

    if (region_length < (offset + length)) {
        return UCS_ERR_NO_RESOURCE;
    }

    if (base_p != NULL) {
        *base_p = UCS_PTR_BYTE_OFFSET(region_base, offset);
    }
    if (offset_p != NULL) {
        *offset_p = offset;
    }
    if (stride_p != NULL) {
        *stride_p = stride;
    }
    if (length_p != NULL) {
        *length_p = length;
    }
    return UCS_OK;
}


static UCS_F_ALWAYS_INLINE void *
uct_obmm_nc_bulk_ctrl_slot(void *base, size_t stride, unsigned index)
{
    return UCS_PTR_BYTE_OFFSET(base, (size_t)index * stride);
}


static UCS_F_ALWAYS_INLINE size_t
uct_obmm_cc_local_desc_stride(void)
{
    return UCT_OBMM_CC_LOCAL_DESC_PREFIX + UCT_OBMM_CC_LOCAL_BCOPY_SEG_SIZE;
}


static UCS_F_ALWAYS_INLINE void *
uct_obmm_cc_local_desc_data(void *descs, unsigned desc_index)
{
    return UCS_PTR_BYTE_OFFSET(uct_obmm_slot_desc_ptr(
                                       descs, uct_obmm_cc_local_desc_stride(),
                                       desc_index),
                               UCT_OBMM_CC_LOCAL_DESC_PREFIX);
}


static UCS_F_ALWAYS_INLINE uct_obmm_cc_recv_desc_meta_t *
uct_obmm_cc_local_desc_meta_from_chunk(void *chunk)
{
    return (uct_obmm_cc_recv_desc_meta_t*)chunk;
}


static UCS_F_ALWAYS_INLINE uct_obmm_cc_recv_desc_meta_t *
uct_obmm_cc_local_desc_meta_from_uct_desc(void *desc, size_t rx_headroom)
{
    return (uct_obmm_cc_recv_desc_meta_t*)
           UCS_PTR_BYTE_OFFSET(desc,
                               -((ptrdiff_t)UCT_OBMM_CC_LOCAL_DESC_PREFIX -
                                 (ptrdiff_t)rx_headroom));
}


static UCS_F_ALWAYS_INLINE ucs_status_t
uct_obmm_cc_local_layout(size_t *slot_stride_p, size_t *pool_size_p,
                         size_t *prefix_size_p)
{
    size_t slot_stride = uct_obmm_slot_stride_descs(
            UCT_OBMM_CC_LOCAL_FIFO_SIZE, UCT_OBMM_CC_LOCAL_FIFO_ELEM_SIZE,
            UCT_OBMM_CC_LOCAL_DESC_COUNT, uct_obmm_cc_local_desc_stride());
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
