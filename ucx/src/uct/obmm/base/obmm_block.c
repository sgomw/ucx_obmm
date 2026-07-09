/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "obmm_block.h"
#include "obmm_atomic.h"

#include <ucs/arch/cpu.h>
#include <ucs/debug/log.h>
#include <ucs/sys/math.h>
#include <ucs/sys/sys.h>

#include <inttypes.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>


#define UCT_OBMM_BLOCK_INIT_SPIN_LIMIT   (1u << 22)
#define UCT_OBMM_BLOCK_COLOR_ALIGN       UCS_SYS_CACHE_LINE_SIZE
#define UCT_OBMM_BLOCK_COLOR_GRANULARITY (2 * UCS_MBYTE)


static int uct_obmm_block_proc_alive(uint32_t pid, uint64_t starttime)
{
    unsigned long live;

    if (pid == 0) {
        return 0;
    }

    live = ucs_sys_get_proc_create_time((pid_t)pid);
    if (live == 0ul) {
        return 0;
    }

    return (uint64_t)live == starttime;
}


static int uct_obmm_block_owner_live(uct_obmm_block_hdr_t *hdr)
{
    uint32_t owner_pid;
    uint64_t owner_starttime;

    ucs_memory_bus_load_fence();
    owner_pid       = hdr->owner_pid;
    owner_starttime = hdr->owner_starttime;
    return uct_obmm_block_proc_alive(owner_pid, owner_starttime);
}


static int uct_obmm_block_mem_has_data(const void *ptr, size_t length)
{
    const unsigned char *p = ptr;
    size_t               i;

    for (i = 0; i < length; ++i) {
        if (p[i] != 0) {
            return 1;
        }
    }

    return 0;
}


static int
uct_obmm_block_range_has_data(const char *base, size_t length, size_t start,
                              size_t end)
{
    if (start >= length) {
        return 0;
    }

    if (end > length) {
        end = length;
    }

    if (end <= start) {
        return 0;
    }

    return uct_obmm_block_mem_has_data(base + start, end - start);
}


static int uct_obmm_block_metadata_has_data(uct_obmm_block_hdr_t *hdr,
                                            size_t region_size)
{
    char  *base                   = (char*)hdr;
    size_t length                 = ucs_min(uct_obmm_block_min_fifo_offset(),
                                            region_size);
    size_t state_end              = offsetof(uct_obmm_block_hdr_t, state) +
                                    sizeof(hdr->state);
    size_t owner_pid_offset       = offsetof(uct_obmm_block_hdr_t, owner_pid);
    size_t owner_pid_end          = owner_pid_offset + sizeof(hdr->owner_pid);
    size_t owner_starttime_offset = offsetof(uct_obmm_block_hdr_t,
                                             owner_starttime);
    size_t owner_starttime_end    = owner_starttime_offset +
                                    sizeof(hdr->owner_starttime);

    return uct_obmm_block_range_has_data(base, length, state_end,
                                         owner_pid_offset) ||
           uct_obmm_block_range_has_data(base, length, owner_pid_end,
                                         owner_starttime_offset) ||
           uct_obmm_block_range_has_data(base, length,
                                         owner_starttime_end, length);
}


static void uct_obmm_block_clear_keep_state(uct_obmm_block_hdr_t *hdr,
                                            size_t length)
{
    char  *base         = (char*)hdr;
    size_t state_offset = offsetof(uct_obmm_block_hdr_t, state);
    size_t state_end    = state_offset + sizeof(hdr->state);

    if (state_offset > 0) {
        memset(base, 0, state_offset);
    }
    if (length > state_end) {
        memset(base + state_end, 0, length - state_end);
    }
}


static void
uct_obmm_block_zero_range(char *base, size_t length, size_t start, size_t end)
{
    if (start >= length) {
        return;
    }

    if (end > length) {
        end = length;
    }

    if (end > start) {
        memset(base + start, 0, end - start);
    }
}


static void
uct_obmm_block_clear_keep_state_owner(uct_obmm_block_hdr_t *hdr, size_t length)
{
    char  *base                   = (char*)hdr;
    size_t state_offset           = offsetof(uct_obmm_block_hdr_t, state);
    size_t state_end              = state_offset + sizeof(hdr->state);
    size_t owner_pid_offset       = offsetof(uct_obmm_block_hdr_t, owner_pid);
    size_t owner_pid_end          = owner_pid_offset + sizeof(hdr->owner_pid);
    size_t owner_starttime_offset = offsetof(uct_obmm_block_hdr_t,
                                             owner_starttime);
    size_t owner_starttime_end    = owner_starttime_offset +
                                    sizeof(hdr->owner_starttime);

    uct_obmm_block_zero_range(base, length, 0, state_offset);
    uct_obmm_block_zero_range(base, length, state_end, owner_pid_offset);
    uct_obmm_block_zero_range(base, length, owner_pid_end,
                              owner_starttime_offset);
    uct_obmm_block_zero_range(base, length, owner_starttime_end, length);
}


size_t uct_obmm_block_min_fifo_offset(void)
{
    return ucs_align_up(sizeof(uct_obmm_block_hdr_t),
                        UCS_SYS_CACHE_LINE_SIZE);
}


static size_t uct_obmm_block_color_step_units(uint32_t fifo_stride)
{
    size_t step = ucs_align_down((size_t)fifo_stride %
                                 UCT_OBMM_BLOCK_COLOR_GRANULARITY,
                                 UCT_OBMM_BLOCK_COLOR_ALIGN);

    return (step == 0) ? 1 : (step / UCT_OBMM_BLOCK_COLOR_ALIGN);
}


size_t uct_obmm_block_colored_fifo_offset(uint32_t fifo_stride,
                                          size_t region_size,
                                          uint32_t region_id)
{
    size_t   min_offset = uct_obmm_block_min_fifo_offset();
    size_t   min_required;
    size_t   color_span;
    size_t   color_positions;
    size_t   color_unit;
    size_t   step_units;
    uint64_t color_key;

    if (fifo_stride > (SIZE_MAX - min_offset)) {
        return min_offset;
    }

    min_required = min_offset + fifo_stride;
    if ((region_id == UCT_OBMM_BLOCK_COLOR_ID_NONE) ||
        (region_size <= min_required)) {
        return min_offset;
    }

    color_span      = region_size - min_required;
    color_positions = (color_span / UCT_OBMM_BLOCK_COLOR_ALIGN) + 1u;
    if (color_positions <= 1) {
        return min_offset;
    }

    step_units = uct_obmm_block_color_step_units(fifo_stride);
    color_key  = (uint64_t)region_id * step_units;
    color_unit = (size_t)(color_key % color_positions);
    return min_offset + color_unit * UCT_OBMM_BLOCK_COLOR_ALIGN;
}


size_t uct_obmm_block_required_size(uint32_t fifo_stride,
                                    size_t fifo_offset)
{
    if (fifo_stride > (SIZE_MAX - fifo_offset)) {
        return SIZE_MAX;
    }

    return fifo_offset + fifo_stride;
}


static ucs_status_t
uct_obmm_block_validate_layout(size_t region_size, uint32_t fifo_stride,
                               size_t fifo_offset, size_t *required_p)
{
    size_t min_offset;
    size_t required;

    if (fifo_stride == 0) {
        return UCS_ERR_INVALID_PARAM;
    }

    min_offset = uct_obmm_block_min_fifo_offset();
    if ((fifo_offset < min_offset) ||
        ((fifo_offset % UCS_SYS_CACHE_LINE_SIZE) != 0)) {
        ucs_error("obmm: invalid FIFO offset %zu (min=%zu align=%u "
                  "stride=%u)",
                  fifo_offset, min_offset, (unsigned)UCS_SYS_CACHE_LINE_SIZE,
                  fifo_stride);
        return UCS_ERR_INVALID_PARAM;
    }

    required = uct_obmm_block_required_size(fifo_stride, fifo_offset);
    if (required == SIZE_MAX) {
        ucs_error("obmm: FIFO block size overflows (offset=%zu stride=%u)",
                  fifo_offset, fifo_stride);
        return UCS_ERR_INVALID_PARAM;
    }

    if (region_size < required) {
        ucs_error("obmm: region size %zu < required FIFO block size %zu "
                  "(offset=%zu stride=%u)",
                  region_size, required, fifo_offset, fifo_stride);
        return UCS_ERR_BUFFER_TOO_SMALL;
    }

    *required_p = required;
    return UCS_OK;
}


static ucs_status_t
uct_obmm_block_validate_ready(uct_obmm_block_hdr_t *hdr, size_t region_size,
                              uint32_t fifo_stride,
                              size_t *fifo_offset_p)
{
    uint32_t     hdr_state;
    uint64_t     hdr_fifo_offset_u64;
    size_t       hdr_fifo_offset;
    size_t       required;
    ucs_status_t status;

    hdr_state = hdr->state;
    ucs_memory_bus_load_fence();

    if (hdr_state != UCT_OBMM_BLOCK_STATE_READY) {
        ucs_debug("obmm: FIFO block at %p not READY (state=%u)", hdr,
                  hdr_state);
        return UCS_ERR_NO_RESOURCE;
    }

    hdr_fifo_offset_u64 = hdr->fifo_offset;
    if (hdr_fifo_offset_u64 > SIZE_MAX) {
        ucs_error("obmm: FIFO offset overflows size_t: %" PRIu64,
                  hdr_fifo_offset_u64);
        return UCS_ERR_INVALID_PARAM;
    }

    hdr_fifo_offset = (size_t)hdr_fifo_offset_u64;
    status = uct_obmm_block_validate_layout(region_size, fifo_stride,
                                            hdr_fifo_offset, &required);
    if (status != UCS_OK) {
        return status;
    }

    *fifo_offset_p = hdr_fifo_offset;
    (void)required;
    return UCS_OK;
}


static void
uct_obmm_block_publish_ready(uct_obmm_block_hdr_t *hdr, size_t region_size,
                             size_t fifo_offset,
                             const char *reason)
{
    int stale_metadata;

    stale_metadata = uct_obmm_block_metadata_has_data(hdr, region_size);
    if (stale_metadata) {
        ucs_warn("obmm: stale FIFO block metadata before init (%s); "
                 "clearing shared region and reinitializing", reason);
    }

    uct_obmm_block_clear_keep_state_owner(hdr, region_size);
    ucs_memory_bus_store_fence();

    hdr->owner_pid       = (uint32_t)getpid();
    hdr->owner_starttime = (uint64_t)ucs_sys_get_proc_create_time(getpid());
    hdr->fifo_offset     = fifo_offset;

    ucs_memory_bus_store_fence();
    hdr->state = UCT_OBMM_BLOCK_STATE_READY;
}


static void uct_obmm_block_set_init_owner(uct_obmm_block_hdr_t *hdr)
{
    hdr->owner_pid       = (uint32_t)getpid();
    hdr->owner_starttime = (uint64_t)ucs_sys_get_proc_create_time(getpid());
    ucs_memory_bus_store_fence();
}


static ucs_status_t
uct_obmm_block_claim(uct_obmm_block_hdr_t *hdr, size_t region_size,
                     size_t fifo_offset)
{
    uint32_t state;
    uint32_t prev;
    uint32_t owner_pid;
    uint64_t owner_starttime;
    unsigned spin;

retry:
    prev = uct_obmm_atomic_cswap32(&hdr->state,
                                   UCT_OBMM_BLOCK_STATE_UNINIT,
                                   UCT_OBMM_BLOCK_STATE_INITING);
    if (prev == UCT_OBMM_BLOCK_STATE_UNINIT) {
        uct_obmm_block_set_init_owner(hdr);
        uct_obmm_block_publish_ready(hdr, region_size, fifo_offset,
                                     "UNINIT header not clean");
        return UCS_OK;
    }

    if (prev == UCT_OBMM_BLOCK_STATE_READY) {
        if (uct_obmm_block_owner_live(hdr)) {
            return UCS_ERR_NO_RESOURCE;
        }

        prev = uct_obmm_atomic_cswap32(&hdr->state,
                                       UCT_OBMM_BLOCK_STATE_READY,
                                       UCT_OBMM_BLOCK_STATE_INITING);
        if (prev == UCT_OBMM_BLOCK_STATE_READY) {
            uct_obmm_block_set_init_owner(hdr);
            uct_obmm_block_publish_ready(hdr, region_size, fifo_offset,
                                         "READY block has no live owner");
            return UCS_OK;
        }
        goto retry;
    }

    for (spin = 0; spin < UCT_OBMM_BLOCK_INIT_SPIN_LIMIT; ++spin) {
        state = hdr->state;
        ucs_memory_bus_load_fence();
        if (state == UCT_OBMM_BLOCK_STATE_READY) {
            if (uct_obmm_block_owner_live(hdr)) {
                return UCS_ERR_NO_RESOURCE;
            }
            goto retry;
        }
        if (state == UCT_OBMM_BLOCK_STATE_UNINIT) {
            goto retry;
        }
        if (state != UCT_OBMM_BLOCK_STATE_INITING) {
            prev = uct_obmm_atomic_cswap32(&hdr->state, state,
                                           UCT_OBMM_BLOCK_STATE_UNINIT);
            if (prev == state) {
                ucs_warn("obmm: invalid FIFO block init state %u; "
                         "resetting init state", state);
            }
            goto retry;
        }

        if ((spin & 0xfffu) == 0xfffu) {
            ucs_memory_bus_load_fence();
            owner_pid       = hdr->owner_pid;
            owner_starttime = hdr->owner_starttime;
            if ((owner_pid != 0) && (owner_starttime != 0) &&
                !uct_obmm_block_proc_alive(owner_pid, owner_starttime)) {
                prev = uct_obmm_atomic_cswap32(
                        &hdr->state, UCT_OBMM_BLOCK_STATE_INITING,
                        UCT_OBMM_BLOCK_STATE_UNINIT);
                if (prev == UCT_OBMM_BLOCK_STATE_INITING) {
                    ucs_warn("obmm: FIFO block initializer pid=%u is not "
                             "live; resetting init state", owner_pid);
                }
                goto retry;
            }
        }
    }

    ucs_error("obmm: timed out waiting for FIFO block init to complete "
              "(owner pid=%u)", hdr->owner_pid);
    return UCS_ERR_TIMED_OUT;
}


static void uct_obmm_block_fill(uct_obmm_block_t *block, void *region_base,
                                size_t region_size, size_t fifo_offset,
                                uint32_t fifo_stride)
{
    block->base        = region_base;
    block->length      = region_size;
    block->hdr         = (uct_obmm_block_hdr_t*)region_base;
    block->fifo        = (char*)region_base + fifo_offset;
    block->ctl         = uct_obmm_fifo_ctl(block->fifo);
    block->elems       = uct_obmm_fifo_elems(block->fifo);
    block->fifo_offset = fifo_offset;
    block->fifo_stride = fifo_stride;
}


ucs_status_t uct_obmm_block_attach(void *region_base, size_t region_size,
                                   uint32_t fifo_stride,
                                   size_t fifo_offset,
                                   uct_obmm_block_t *block)
{
    uct_obmm_block_hdr_t *hdr = (uct_obmm_block_hdr_t*)region_base;
    size_t                required;
    ucs_status_t          status;

    status = uct_obmm_block_validate_layout(region_size, fifo_stride,
                                            fifo_offset, &required);
    if (status != UCS_OK) {
        return status;
    }

    status = uct_obmm_block_claim(hdr, region_size, fifo_offset);
    if (status != UCS_OK) {
        return status;
    }

    status = uct_obmm_block_validate_ready(hdr, region_size, fifo_stride,
                                           &fifo_offset);
    if (status != UCS_OK) {
        return status;
    }

    uct_obmm_block_fill(block, region_base, region_size, fifo_offset,
                        fifo_stride);
    (void)required;
    return UCS_OK;
}


ucs_status_t uct_obmm_block_open(void *region_base, size_t region_size,
                                 uint32_t fifo_stride,
                                 uct_obmm_block_t *block)
{
    uct_obmm_block_hdr_t *hdr = (uct_obmm_block_hdr_t*)region_base;
    size_t                fifo_offset;
    ucs_status_t          status;

    status = uct_obmm_block_validate_ready(hdr, region_size, fifo_stride,
                                           &fifo_offset);
    if (status != UCS_OK) {
        return status;
    }

    uct_obmm_block_fill(block, region_base, region_size, fifo_offset,
                        fifo_stride);
    return UCS_OK;
}


void uct_obmm_block_release(uct_obmm_block_t *block)
{
    uct_obmm_block_hdr_t *hdr;
    unsigned long         self_starttime;
    uint32_t              prev;

    if ((block == NULL) || (block->hdr == NULL) || (block->base == NULL)) {
        return;
    }

    hdr = block->hdr;
    self_starttime = ucs_sys_get_proc_create_time(getpid());
    ucs_memory_bus_load_fence();
    if ((hdr->owner_pid != (uint32_t)getpid()) ||
        (hdr->owner_starttime != (uint64_t)self_starttime)) {
        ucs_warn("obmm: refusing to release FIFO block owned by pid=%u",
                 hdr->owner_pid);
        memset(block, 0, sizeof(*block));
        return;
    }

    prev = uct_obmm_atomic_cswap32(&hdr->state, UCT_OBMM_BLOCK_STATE_READY,
                                   UCT_OBMM_BLOCK_STATE_INITING);
    if (prev != UCT_OBMM_BLOCK_STATE_READY) {
        memset(block, 0, sizeof(*block));
        return;
    }

    uct_obmm_block_clear_keep_state(hdr, block->length);
    ucs_memory_bus_store_fence();

    hdr->state = UCT_OBMM_BLOCK_STATE_UNINIT;
    ucs_memory_bus_store_fence();
    memset(block, 0, sizeof(*block));
}
