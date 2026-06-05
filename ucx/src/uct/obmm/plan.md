# OBMM Plan: NC Short-First Tuning

Status as of 2026-06-05: cross-node cacheable CC is rejected as an obmm UCT
transport route. The active plan is NC-only, AM-only, short-first.

## Final CC Decision

Do not continue staged CC zcopy, sender-owned CC, receiver-owned CC, or
batch/epoch CC as UCT transport paths.

Reasons:

- Sender-owned CC and receiver-owned CC both paid ownership transitions,
  writeback/invalidation, software control messages, and ACK/credit overhead.
- Receiver-owned zcopy briefly improved low-concurrency 4 MiB latency, but
  high-concurrency OSU `multi_lat` regressed and diagnostics showed the
  bottleneck was the per-message ownership/copy/callback/ACK lifecycle.
- Effective ownership granularity behaves PMD/2 MiB-like on the target, so
  small fragments and headers still pay large-range transition costs.
- Batch probing showed speedup only when enough messages are grouped, which
  changes the protocol into a throughput path and requires large windows.
- UCP AM zcopy does not naturally provide the batching needed to amortize CC
  ownership while preserving latency semantics.

The cleanup target is now complete when `ucx_info -d -t obmm` no longer shows
`am_zcopy` and `ucx_info -c` no longer shows `UCX_OBMM_CC_*`.

## Active NC Plan

1. Keep the NC inline FIFO as the primary performance path.
2. Keep `am_bcopy` small and use it for UCP wireup/control/fallback.
3. Keep `uct_obmm_iface_estimate_perf()` and tune its NC short/bcopy cost model
   from measured data.
4. Keep UCP protocol-selection logging so each OSU size can show which UCP
   protocol was selected without broad debug-log noise.
5. Validate FIFO-only short routing and geometry changes on the target
   two-node setup.

## Current Defaults

```text
FIFO_SIZE       = 64
FIFO_ELEM_SIZE  = 520128
BCOPY_SEG_SIZE  = 4096
slot_count      = 96
required_nc     = 3,220,846,912 bytes = 3071.639 MiB
max_short       = 520112 total AM bytes
max_bcopy       = 4096 bytes
```

Prefer 64-byte-aligned FIFO element and bcopy segment sizes unless new
measurements prove otherwise.

## Tuning Questions

- Does increasing `FIFO_ELEM_SIZE` continue to reduce latency up to the point
  where UCP no longer gains from a larger single-fragment short cap?
- Should `SHORT_OVERHEAD`, `BCOPY_OVERHEAD`, and `BW` be adjusted so UCP picks
  NC short/bcopy protocols naturally without explicit user thresholds?
- Is `BCOPY_SEG_SIZE=4096` still the right UCP floor, or should it be reduced
  after confirming wireup/control behavior?
- Is `PENDING_QUOTA=1` still optimal under high process counts?

## Diagnostics

Use `UCX_PROTO_SELECT_LOG=y` and `UCX_PROTO_SELECT_LOG_RANK=<rank>` to inspect
one-shot UCP protocol choices for obmm lanes. Ask the user for only the one to
three relevant log lines or fields needed for each diagnosis.

No cleanup-time performance/statistics log knobs should be added to obmm.
