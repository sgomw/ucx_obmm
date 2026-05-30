/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_STATS_H_
#define UCT_OBMM_STATS_H_

#include <ucs/time/time.h>
#include <stdint.h>


/* Baseline transport counters, aggregated per-iface.
 * All updates are guarded by iface->stats_enable at the call site. */
typedef struct uct_obmm_baseline_stats {
    uint64_t tx_msgs;
    uint64_t tx_bytes;
    uint64_t tx_short_msgs;
    uint64_t tx_bcopy_msgs;
    uint64_t tx_cas_retries;
    uint64_t tx_fifo_full;
    uint64_t pending_queued;
    uint64_t pending_completed;
    uint64_t pending_inprogress;
    uint64_t pending_resched_nores;
    uint64_t pending_resched_retry;
    uint64_t progress_calls;
    uint64_t progress_empty;
    uint64_t rx_msgs;
    uint64_t rx_bytes;
    uint64_t rx_stale_drops;
    uint64_t pending_dispatch_calls;
    uint64_t pending_dispatch_progress;
    uint64_t max_batch;
    uint64_t poll_quota_peak;
} uct_obmm_baseline_stats_t;


/* 1-byte am_short micro-benchmark timing counters.
 * All updates are guarded by iface->short_perf_enable at the call site. */
typedef struct uct_obmm_short_perf_stats {
    uint64_t tx_1b_msgs;
    uint64_t tx_1b_nores;
    uint64_t tx_1b_total_ticks;
    uint64_t tx_1b_copy_ticks;
    uint64_t tx_1b_publish_ticks;
    uint64_t rx_1b_msgs;
    uint64_t rx_1b_progress_calls;
    uint64_t rx_1b_publishes;
    uint64_t rx_1b_total_ticks;
    uint64_t rx_1b_copy_cb_ticks;
    uint64_t rx_1b_publish_ticks;
} uct_obmm_short_perf_stats_t;


void uct_obmm_stats_dump_baseline(const uct_obmm_baseline_stats_t *s);
void uct_obmm_stats_dump_short_perf(const uct_obmm_short_perf_stats_t *s);

#endif
