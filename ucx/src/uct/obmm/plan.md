# OBMM Plan: Dual-Plane Extreme Path

Status as of 2026-06-05: cross-node cacheable CC remains rejected, but
same-node cacheable CC is now an approved fast path. The target architecture is
two logical UCT TLS over one OBMM component:

```text
obmm_cc: same-node AM over cacheable CC export memory
obmm_nc: cross-node AM over non-cacheable NC import/export memory
```

Experiment as of 2026-06-12: measure whether the shared worker-level OBMM
progress callback couples NC/CC empty polling. The current experiment branch
uses normal per-iface UCX progress callbacks for `obmm_nc` and `obmm_cc`
instead of one shared OBMM worker callback. This is not yet the accepted
direction; compare target benchmark data against the shared-callback baseline.

UCP should see separate logical transports so reachability and performance
models are not mixed. The UCT layer should use one shared per-worker OBMM
progress engine so enabling both TLS does not create two independent callback
queue entries.

## Current Direction

1. Register `obmm_nc` and `obmm_cc` as separate TLS under the `obmm` component.
2. Classify shmdevs by user-provided `OBMM_NC_MEMIDS` and `OBMM_CC_MEMIDS`.
   The hardware does not expose NC/CC type to the transport, so any configured
   plane list is user-classified. `CC_MEMIDS` may be provided alone for
   same-node-only `obmm_cc`.
3. Keep `obmm_nc` cross-node-first: remote peers require mapped NC imports.
   Same-node NC loopback is enabled only when no local CC export is configured,
   so standalone `obmm_nc` can run without polluting dual-plane local selection.
4. Keep `obmm_cc` reachable only when the peer address names the same local CC
   export region, so CC is never used cross-node.
5. Share one worker-level OBMM progress callback across active `obmm_nc` and
   `obmm_cc` ifaces.
6. Preserve UCP protocol-selection logging and `uct_obmm_iface_estimate_perf()`
   for both planes.
7. Use one shared FIFO element data area for short and bcopy. The latest
   cross-node measurements show 128 KiB bcopy fragments recover large-message
   performance, while much larger bcopy caps create a wide eager single-bcopy
   range without improving 2-4 MiB latency.

## Rejected Cross-Node CC Direction

Do not revive staged CC zcopy, sender-owned CC, receiver-owned CC, or
batch/epoch CC as cross-node UCT paths.

Reasons:

- Sender-owned and receiver-owned cross-node CC both paid ownership
  transitions, writeback/invalidation, software control messages, and
  ACK/credit overhead.
- Receiver-owned zcopy briefly improved low-concurrency 4 MiB latency, but
  high-concurrency OSU `multi_lat` regressed and diagnostics showed the
  bottleneck was the per-message ownership/copy/callback/ACK lifecycle.
- Effective ownership granularity behaves PMD/2 MiB-like on the target, so
  small fragments and headers still pay large-range transition costs.
- Batch probing showed speedup only when enough messages are grouped, which
  changes the protocol into a throughput path and requires large windows.
- UCP AM zcopy does not naturally provide the batching needed to amortize
  ownership while preserving latency semantics.

## Current Defaults

`obmm_nc`:

```text
FIFO_SIZE       = 128
FIFO_ELEM_SIZE  = 131200
BCOPY_SEG_SIZE  = 131072
BW              = 3400MBs
SHORT_OVERHEAD  = 1800ns
BCOPY_OVERHEAD  = 2us
max_short       = 131184 total AM bytes
max_bcopy       = 131072 bytes
```

`obmm_cc`:

```text
FIFO_SIZE       = 128
FIFO_ELEM_SIZE  = 131200
BCOPY_SEG_SIZE  = 131072
BW              = 12300MBs
SHORT_OVERHEAD  = 100ns
BCOPY_OVERHEAD  = 200ns
max_short       = 131184 total AM bytes
max_bcopy       = 131072 bytes
```

Both planes use 96 slots. Short and bcopy reuse one FIFO element allocation
with overlapping ranges: short starts at byte 16 and bcopy starts at byte 64.
`BCOPY_SEG_SIZE` is an advertised cap rather than an additive per-entry desc
allocation. This restores the measured-fast short layout while retaining
64-byte alignment for large bcopy fragments.
The default geometry requires 1,612,200,256 bytes (1537.514 MiB) per plane.
Prefer 64-byte-aligned FIFO element and bcopy segment sizes unless new
measurements prove otherwise.

## Diagnostics

Use `UCX_PROTO_SELECT_LOG=y` and `UCX_PROTO_SELECT_LOG_RANK=<rank>` to inspect
one-shot UCP protocol choices for OBMM lanes. Ask the user for only the one to
three relevant log lines or fields needed for each diagnosis.

No cleanup-time performance/statistics log knobs should be added to obmm.
