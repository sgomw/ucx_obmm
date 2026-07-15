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


#define UCT_OBMM_BLOCK_COLOR_ALIGN       UCS_SYS_CACHE_LINE_SIZE
#define UCT_OBMM_BLOCK_COLOR_GRANULARITY (2 * UCS_MBYTE)
#define UCT_OBMM_BLOCK_CLAIM_READY       (UINT64_C(1) << 63)
#define UCT_OBMM_BLOCK_CLAIM_PID_BITS    23u
#define UCT_OBMM_BLOCK_CLAIM_PID_MASK \
    ((UINT64_C(1) << UCT_OBMM_BLOCK_CLAIM_PID_BITS) - 1u)
#define UCT_OBMM_BLOCK_CLAIM_TIME_SHIFT  UCT_OBMM_BLOCK_CLAIM_PID_BITS
#define UCT_OBMM_BLOCK_CLAIM_TIME_BITS \
    (63u - UCT_OBMM_BLOCK_CLAIM_PID_BITS)
#define UCT_OBMM_BLOCK_CLAIM_TIME_MASK \
    ((UINT64_C(1) << UCT_OBMM_BLOCK_CLAIM_TIME_BITS) - 1u)


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


static uint32_t uct_obmm_block_claim_pid(uint64_t claim)
{
    return (uint32_t)(claim & UCT_OBMM_BLOCK_CLAIM_PID_MASK);
}


static uint64_t uct_obmm_block_claim_starttime(uint64_t claim)
{
    return (claim >> UCT_OBMM_BLOCK_CLAIM_TIME_SHIFT) &
           UCT_OBMM_BLOCK_CLAIM_TIME_MASK;
}


static uint64_t uct_obmm_block_claim_token(uint32_t pid, uint64_t starttime)
{
    return ((starttime & UCT_OBMM_BLOCK_CLAIM_TIME_MASK) <<
            UCT_OBMM_BLOCK_CLAIM_TIME_SHIFT) |
           ((uint64_t)pid & UCT_OBMM_BLOCK_CLAIM_PID_MASK);
}


static int uct_obmm_block_claim_is_ready(uint64_t claim)
{
    return (claim & UCT_OBMM_BLOCK_CLAIM_READY) != 0;
}


static int uct_obmm_block_claim_is_valid_token(uint64_t claim)
{
    uint64_t token = claim & ~UCT_OBMM_BLOCK_CLAIM_READY;

    return (uct_obmm_block_claim_pid(token) != 0) &&
           (uct_obmm_block_claim_starttime(token) != 0);
}


static int uct_obmm_block_claim_live(uint64_t claim)
{
    uint64_t token = claim & ~UCT_OBMM_BLOCK_CLAIM_READY;

    if (!uct_obmm_block_claim_is_valid_token(token)) {
        return 0;
    }

    return uct_obmm_block_proc_alive(
            uct_obmm_block_claim_pid(token),
            uct_obmm_block_claim_starttime(token));
}


static ucs_status_t uct_obmm_block_make_self_claim(uint64_t *claim_p)
{
    pid_t         pid       = getpid();
    unsigned long starttime = ucs_sys_get_proc_create_time(pid);

    if (((uint64_t)pid > UCT_OBMM_BLOCK_CLAIM_PID_MASK) ||
        (starttime == 0ul) ||
        ((uint64_t)starttime > UCT_OBMM_BLOCK_CLAIM_TIME_MASK)) {
        ucs_error("obmm: cannot encode FIFO block claim pid=%ld "
                  "starttime=%lu", (long)pid, starttime);
        return UCS_ERR_INVALID_PARAM;
    }

    *claim_p = uct_obmm_block_claim_token((uint32_t)pid,
                                          (uint64_t)starttime);
    return UCS_OK;
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
    char  *base      = (char*)hdr;
    size_t length    = ucs_min(uct_obmm_block_min_fifo_offset(), region_size);
    size_t claim_end = offsetof(uct_obmm_block_hdr_t, claim) +
                       sizeof(hdr->claim);

    return uct_obmm_block_range_has_data(base, length, claim_end, length);
}


static void uct_obmm_block_clear_keep_claim(uct_obmm_block_hdr_t *hdr,
                                            size_t length)
{
    char  *base         = (char*)hdr;
    size_t claim_offset = offsetof(uct_obmm_block_hdr_t, claim);
    size_t claim_end    = claim_offset + sizeof(hdr->claim);

    if (claim_offset > 0) {
        memset(base, 0, claim_offset);
    }
    if (length > claim_end) {
        memset(base + claim_end, 0, length - claim_end);
    }
}




size_t uct_obmm_block_min_fifo_offset(void)
{
    return ucs_align_up(sizeof(uct_obmm_block_hdr_t),
                        UCS_SYS_CACHE_LINE_SIZE);
}


static size_t uct_obmm_block_color_step_units(size_t fifo_stride)
{
    size_t step = ucs_align_down(fifo_stride %
                                 UCT_OBMM_BLOCK_COLOR_GRANULARITY,
                                 UCT_OBMM_BLOCK_COLOR_ALIGN);

    return (step == 0) ? 1 : (step / UCT_OBMM_BLOCK_COLOR_ALIGN);
}


size_t uct_obmm_block_colored_fifo_offset(size_t fifo_stride,
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


size_t uct_obmm_block_required_size(size_t fifo_stride,
                                    size_t fifo_offset)
{
    if (fifo_stride > (SIZE_MAX - fifo_offset)) {
        return SIZE_MAX;
    }

    return fifo_offset + fifo_stride;
}


static ucs_status_t
uct_obmm_block_validate_layout(size_t region_size, size_t fifo_stride,
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
                  "stride=%zu)",
                  fifo_offset, min_offset, (unsigned)UCS_SYS_CACHE_LINE_SIZE,
                  fifo_stride);
        return UCS_ERR_INVALID_PARAM;
    }

    required = uct_obmm_block_required_size(fifo_stride, fifo_offset);
    if (required == SIZE_MAX) {
        ucs_error("obmm: FIFO block size overflows (offset=%zu stride=%zu)",
                  fifo_offset, fifo_stride);
        return UCS_ERR_INVALID_PARAM;
    }

    if (region_size < required) {
        ucs_error("obmm: region size %zu < required FIFO block size %zu "
                  "(offset=%zu stride=%zu)",
                  region_size, required, fifo_offset, fifo_stride);
        return UCS_ERR_BUFFER_TOO_SMALL;
    }

    *required_p = required;
    return UCS_OK;
}


static ucs_status_t
uct_obmm_block_validate_ready(uct_obmm_block_hdr_t *hdr, size_t region_size,
                              size_t fifo_stride,
                              size_t *fifo_offset_p)
{
    uint64_t     hdr_claim;
    uint64_t     hdr_fifo_offset_u64;
    size_t       hdr_fifo_offset;
    size_t       required;
    ucs_status_t status;

    hdr_claim = hdr->claim;
    ucs_memory_bus_load_fence();

    if (!uct_obmm_block_claim_is_ready(hdr_claim) ||
        !uct_obmm_block_claim_is_valid_token(hdr_claim)) {
        ucs_debug("obmm: FIFO block at %p not READY/valid "
                  "(claim=0x%" PRIx64 ")", hdr, hdr_claim);
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
                             size_t fifo_offset, uint64_t claim,
                             const char *reason)
{
    int stale_metadata;

    stale_metadata = uct_obmm_block_metadata_has_data(hdr, region_size);
    if (stale_metadata) {
        ucs_warn("obmm: stale FIFO block metadata before init (%s); "
                 "clearing shared region and reinitializing", reason);
    }

    uct_obmm_block_clear_keep_claim(hdr, region_size);
    ucs_memory_bus_store_fence();

    hdr->fifo_offset = fifo_offset;

    ucs_memory_bus_store_fence();
    hdr->claim = claim | UCT_OBMM_BLOCK_CLAIM_READY;
}


static ucs_status_t
uct_obmm_block_takeover_claim(uct_obmm_block_hdr_t *hdr, uint64_t observed,
                              uint64_t claim, size_t region_size,
                              size_t fifo_offset, const char *reason)
{
    uint64_t prev;

    prev = uct_obmm_atomic_cswap64(&hdr->claim, observed, claim);
    if (prev != observed) {
        return UCS_INPROGRESS;
    }

    ucs_warn("obmm: FIFO block claim pid=%u is not live; taking over claim",
             uct_obmm_block_claim_pid(observed));
    uct_obmm_block_publish_ready(hdr, region_size, fifo_offset, claim,
                                 reason);
    return UCS_OK;
}


static ucs_status_t
uct_obmm_block_claim(uct_obmm_block_hdr_t *hdr, size_t region_size,
                     size_t fifo_offset)
{
    uint64_t claim;
    uint64_t prev;
    uint64_t observed;
    ucs_status_t status;

    status = uct_obmm_block_make_self_claim(&claim);
    if (status != UCS_OK) {
        return status;
    }

retry:
    prev = uct_obmm_atomic_cswap64(&hdr->claim, 0, claim);
    if (prev == 0) {
        uct_obmm_block_publish_ready(hdr, region_size, fifo_offset, claim,
                                     "UNINIT header not clean");
        return UCS_OK;
    }

    observed = prev;
    if (!uct_obmm_block_claim_is_valid_token(observed)) {
        prev = uct_obmm_atomic_cswap64(&hdr->claim, observed, 0);
        if (prev == observed) {
            ucs_warn("obmm: invalid FIFO block claim 0x%" PRIx64
                     "; resetting claim", observed);
        }
        goto retry;
    }

    if (uct_obmm_block_claim_live(observed)) {
        return UCS_ERR_NO_RESOURCE;
    }

    status = uct_obmm_block_takeover_claim(
            hdr, observed, claim, region_size, fifo_offset,
            uct_obmm_block_claim_is_ready(observed) ?
            "READY block has no live owner" : "claim owner died");
    if (status == UCS_INPROGRESS) {
        goto retry;
    }

    return status;
}


static void uct_obmm_block_fill(uct_obmm_block_t *block, void *region_base,
                                size_t region_size, size_t fifo_offset,
                                size_t fifo_stride)
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
                                   size_t fifo_stride,
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
                                 size_t fifo_stride,
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
    uint64_t              claim;
    uint64_t              ready_claim;
    uint64_t              prev;
    ucs_status_t          status;

    if ((block == NULL) || (block->hdr == NULL) || (block->base == NULL)) {
        return;
    }

    hdr = block->hdr;
    status = uct_obmm_block_make_self_claim(&claim);
    if (status != UCS_OK) {
        memset(block, 0, sizeof(*block));
        return;
    }

    ready_claim = claim | UCT_OBMM_BLOCK_CLAIM_READY;
    ucs_memory_bus_load_fence();
    if (hdr->claim != ready_claim) {
        ucs_warn("obmm: refusing to release FIFO block owned by claim=0x%"
                 PRIx64, hdr->claim);
        memset(block, 0, sizeof(*block));
        return;
    }

    prev = uct_obmm_atomic_cswap64(&hdr->claim, ready_claim, claim);
    if (prev != ready_claim) {
        memset(block, 0, sizeof(*block));
        return;
    }

    uct_obmm_block_clear_keep_claim(hdr, block->length);
    ucs_memory_bus_store_fence();

    hdr->claim = 0;
    ucs_memory_bus_store_fence();
    memset(block, 0, sizeof(*block));
}
