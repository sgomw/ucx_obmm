/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "obmm_stats.h"

#include <ucs/debug/log.h>
#include <ucs/time/time.h>


static double uct_obmm_stats_avg_nsec(uint64_t ticks, uint64_t count)
{
    return (count == 0) ? 0.0 : (ucs_time_to_nsec((ucs_time_t)ticks) / count);
}

void uct_obmm_stats_dump_baseline(const uct_obmm_baseline_stats_t *s)
{
    ucs_warn("obmm-stats tx_msgs=%llu tx_bytes=%llu tx_short=%llu "
             "tx_bcopy=%llu cas_retries=%llu fifo_full=%llu "
             "pending_queued=%llu pending_ok=%llu pending_inprogress=%llu "
             "pending_resched_nores=%llu pending_resched_retry=%llu "
             "progress_calls=%llu progress_empty=%llu rx_msgs=%llu "
             "rx_bytes=%llu stale=%llu pending_dispatch_calls=%llu "
             "pending_dispatch_progress=%llu max_batch=%llu poll_quota_peak=%llu",
             (unsigned long long)s->tx_msgs,
             (unsigned long long)s->tx_bytes,
             (unsigned long long)s->tx_short_msgs,
             (unsigned long long)s->tx_bcopy_msgs,
             (unsigned long long)s->tx_cas_retries,
             (unsigned long long)s->tx_fifo_full,
             (unsigned long long)s->pending_queued,
             (unsigned long long)s->pending_completed,
             (unsigned long long)s->pending_inprogress,
             (unsigned long long)s->pending_resched_nores,
             (unsigned long long)s->pending_resched_retry,
             (unsigned long long)s->progress_calls,
             (unsigned long long)s->progress_empty,
             (unsigned long long)s->rx_msgs,
             (unsigned long long)s->rx_bytes,
             (unsigned long long)s->rx_stale_drops,
             (unsigned long long)s->pending_dispatch_calls,
             (unsigned long long)s->pending_dispatch_progress,
             (unsigned long long)s->max_batch,
             (unsigned long long)s->poll_quota_peak);
}

void uct_obmm_stats_dump_short_perf(const uct_obmm_short_perf_stats_t *s)
{
    uint64_t tx_wait_ticks;
    uint64_t rx_other_ticks;

    tx_wait_ticks = s->tx_1b_total_ticks;
    if (tx_wait_ticks >= s->tx_1b_copy_ticks) {
        tx_wait_ticks -= s->tx_1b_copy_ticks;
    } else {
        tx_wait_ticks = 0;
    }
    if (tx_wait_ticks >= s->tx_1b_publish_ticks) {
        tx_wait_ticks -= s->tx_1b_publish_ticks;
    } else {
        tx_wait_ticks = 0;
    }

    rx_other_ticks = s->rx_1b_total_ticks;
    if (rx_other_ticks >= s->rx_1b_copy_cb_ticks) {
        rx_other_ticks -= s->rx_1b_copy_cb_ticks;
    } else {
        rx_other_ticks = 0;
    }
    if (rx_other_ticks >= s->rx_1b_publish_ticks) {
        rx_other_ticks -= s->rx_1b_publish_ticks;
    } else {
        rx_other_ticks = 0;
    }

    ucs_warn("obmm-short1b tx_msgs=%llu tx_nores=%llu "
             "tx_avg_total_ns=%.2f tx_avg_wait_ns=%.2f "
             "tx_avg_copy_ns=%.2f tx_avg_publish_ns=%.2f "
             "rx_msgs=%llu rx_progress_calls=%llu rx_publishes=%llu "
             "rx_msgs_per_progress=%.2f rx_avg_progress_ns=%.2f "
             "rx_avg_other_ns=%.2f rx_avg_copycb_ns=%.2f "
             "rx_avg_publish_ns_per_msg=%.2f",
             (unsigned long long)s->tx_1b_msgs,
             (unsigned long long)s->tx_1b_nores,
             uct_obmm_stats_avg_nsec(s->tx_1b_total_ticks, s->tx_1b_msgs),
             uct_obmm_stats_avg_nsec(tx_wait_ticks, s->tx_1b_msgs),
             uct_obmm_stats_avg_nsec(s->tx_1b_copy_ticks, s->tx_1b_msgs),
             uct_obmm_stats_avg_nsec(s->tx_1b_publish_ticks, s->tx_1b_msgs),
             (unsigned long long)s->rx_1b_msgs,
             (unsigned long long)s->rx_1b_progress_calls,
             (unsigned long long)s->rx_1b_publishes,
             (s->rx_1b_progress_calls == 0) ? 0.0 :
             ((double)s->rx_1b_msgs / s->rx_1b_progress_calls),
             uct_obmm_stats_avg_nsec(s->rx_1b_total_ticks,
                                     s->rx_1b_progress_calls),
             uct_obmm_stats_avg_nsec(rx_other_ticks,
                                     s->rx_1b_progress_calls),
             uct_obmm_stats_avg_nsec(s->rx_1b_copy_cb_ticks, s->rx_1b_msgs),
             uct_obmm_stats_avg_nsec(s->rx_1b_publish_ticks, s->rx_1b_msgs));
}
