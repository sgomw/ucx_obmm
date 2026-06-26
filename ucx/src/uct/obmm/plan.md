# OBMM Plan: Dual-Plane Extreme Path

Status as of 2026-06-05: cross-node cacheable CC remains rejected, but
same-node cacheable CC is now an approved fast path. The target architecture is
two logical UCT TLS over one OBMM component:

```text
obmm_cc: same-node AM over cacheable CC export memory
obmm_nc: cross-node AM over non-cacheable NC import/export memory
```

Progress simplification as of 2026-06-15: target measurements showed that the
shared worker-level OBMM progress callback and normal per-iface UCX progress
callbacks perform essentially the same for `obmm_nc + obmm_cc`. Keep the
per-iface path because it removes the private worker context, active iface
list, and active-count lifecycle without a measured regression.

Legacy NC-only discovery removal as of 2026-06-22: remove `UCX_OBMM_MEMIDS`
and the no-list NC directory-scan fallback. OBMM MD discovery now requires
`UCX_OBMM_NC_MEMIDS` and/or `UCX_OBMM_CC_MEMIDS`; otherwise the MD reports no
device instead of treating unknown shmdevs as NC. The sysfs helper itself also
requires a non-empty explicit memid list and no longer contains a directory
scan fallback.

NC-only placement diagnosis as of 2026-06-15: interpret OSU `multi_lat` as
half-split rank pairing, not adjacent-rank pairing. With two 70-slot nodes,
`np=70 map-by-slot` and `np=140 map-by-node` both create 35 same-node NC pairs
per node, and their `obmm_nc` curves are nearly identical across the tested
sizes. This makes the same-node NC large-message regression a per-node local NC
data-path wall rather than a mixed-TL artifact. Use
`ucx/src/uct/obmm/probes/obmm_nc_mem_probe.c` to validate whether the wall is
the local NC mmap read/write bandwidth resource before changing FIFO geometry
or UCP cost defaults.

Pool/header cleanup simplification as of 2026-06-16: pool header geometry is
not a hard compatibility gate. On attach, stale metadata from a previous run
is detected by checking the expected metadata area and, when no live owners
exist, warning before clearing the shared region and reinitializing it. Normal
iface cleanup zeroes the owned FIFO slot; final pool cleanup zeroes the full
mapped region before publishing UNINIT. Peer pool open uses the peer iface
address geometry for pointer math instead of validating peer header geometry.
`wire_format` remains the obmm UCT ABI/code guard because OMPI/PML UCX and UCP
worker-address versioning do not prove that both sides loaded the same obmm UCT
code.

Slot generation removal as of 2026-06-22: remove per-slot generation tokens
from pool metadata, iface addresses, and FIFO elements. Slot allocation now
relies on zeroing the complete slot before publishing `IN_USE`; cleanup and
final reset also zero slot/full-region bytes.

Pool magic removal as of 2026-06-24: remove the write-only `magic` word from
the shared pool header. Wire-format exchange cannot protect independently
launched old/new processes that attach the same local export region before they
exchange addresses; deploy one binary version per shared region.

The byte-8 FIFO experiment as of 2026-06-24 regressed large OSU traffic even
after holding `max_short` at 131184. A packed-type A/B produced the same
regression, ruling out removal of `UCS_S_PACKED` as the cause. The active
layout therefore restores `length@4`, `header@16`, short byte 16, and the
24-byte element header used by the prior measured baseline. It represents the
2-byte and 8-byte physical gaps with anonymous padding fields, not semantic
reserved members. FIFO element stride and bcopy byte 64 remain unchanged;
physical and advertised `max_short` are both 131184 total AM bytes. The active
wire format is `UCT_OBMM_WIRE_FORMAT_SHORT16_PAD` (value 11), which rejects
the regressing byte-8 v10 peers.

FIFO-depth sizing as of 2026-06-16: pool/header bytes are not the limiting
factor for doubling `FIFO_SIZE`. With 96 slots, 256 entries, and 128 KiB FIFO
elements, the element arrays alone consume the full 3 GiB NC export before
control/header bytes are counted. A 95-slot/256-FIFO experiment would fit the
current 70-process environment, but it is not acceptable as the commercial
default because commercial deployments must preserve 96 slots. Keep the default
at `slot_count=96`, `FIFO_SIZE=128`, `FIFO_ELEM_SIZE=131200`, and
`BCOPY_SEG_SIZE=131072` unless a new design trades off bcopy cap, element size,
or export size.

UCP should see separate logical transports so reachability and performance
models are not mixed. The UCT layer should use normal per-iface progress for
each active OBMM iface.

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
5. Use normal per-iface UCX progress callbacks for active `obmm_nc` and
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
allocation. This preserves the measured byte-16 short spacing while retaining
64-byte alignment for large bcopy fragments.
The default geometry requires 1,612,200,256 bytes (1537.514 MiB) per plane.
Prefer 64-byte-aligned FIFO element and bcopy segment sizes unless new
measurements prove otherwise.

## Diagnostics

Use `UCX_PROTO_SELECT_LOG=y` and `UCX_PROTO_SELECT_LOG_RANK=<rank>` to inspect
one-shot UCP protocol choices for selected UCT lanes. Ask the user for only the one to
three relevant log lines or fields needed for each diagnosis.

For the NC same-node large-message wall, first run the standalone local memory
probe from `ucx/src/uct/obmm/probes/` outside UCX. A 70-process
`--mode handoff --bytes 4194304` run on one node matches the OSU case with 35
same-node NC producer/consumer pairs more closely than unsynchronized same-
address pressure. If externally summed per-node handoff bandwidth lands on the
same plateau implied by OSU, the limiting resource is local NC mmap payload
movement. If handoff is much faster than OSU, investigate FIFO progress,
pending retry, and UCP protocol thresholds before retuning advertised caps.
Use `--mode pair` only to test whether local NC reads collapse when writers
hammer the same addresses concurrently.

No cleanup-time performance/statistics log knobs should be added to obmm.
