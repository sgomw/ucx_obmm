# OBMM Plan: NC-Only Single TLS

NC-only update as of 2026-07-06: remove the cacheable same-node path and the
internal plane concept from the active transport. `obmm` now has one NC
receive FIFO per iface, one wire address path, and bus-domain fences only.
`UCX_OBMM_SAME_NODE_MEMID` is removed; `UCX_OBMM_MEMIDS` remains an optional
allow-list override.

Auto-discovery update as of 2026-07-06: `UCX_OBMM_MEMIDS` is no longer needed
for the normal block-FIFO NC deployment. When it is omitted, the MD scans
`/sys/devices/obmm/obmm_shmdev*`, reads `priv_len`/`priv`, and admits only
mappable shmdevs whose private metadata is exactly `ucx-obmm:NN`. Local
exports among them become claimable FIFO blocks and imports become peer
mappings.

Slot-coloring experiment as of 2026-07-07: high-concurrency OSU data showed
that the block-FIFO layout regressed small-message latency while `-x` warmup
changes and UCP proto selection did not explain the difference. To test whether
many independent shmdev blocks place FIFO control words at the same
block-relative offset and trigger hardware address-set conflicts, the pool
header remains at region base but `slot_array_offset` is now selected by
hashing the `priv`-derived `region_id` and aligning to 64 bytes. The wire
format advances to `UCT_OBMM_WIRE_FORMAT_NC_ONLY` (value 17), because older
builds assume the fixed slot offset.

Block-FIFO update as of 2026-07-03: the hardware environment now
pre-provisions 96 NC export shmdev blocks per node, and each process claims one
whole export block instead of allocating one slot from a single 96-slot export
region. Remote nodes pre-import each peer node's 96 export blocks, so a two-node
run has 96 imports per node and a four-node run has 96 * 3 imports per node.
Each export/import block contains one FIFO pool slot; with the current
`FIFO_SIZE=256`, `FIFO_ELEM_SIZE=131200`, and `BCOPY_SEG_SIZE=131072`, the
minimum block footprint is 33,587,392 bytes. Because the OBMM allocation
granularity is 2 MiB, provision each block as 34 MiB.

The MD accepts multiple local exports, maps them all, validates that
`(exporter_dcna, exporter_deid, region_id)` is unique, and lets each iface
claim the first free export block. `region_id` is derived from the shmdev
`priv` metadata so peers can distinguish multiple export blocks from the same
node without using local memid as the peer key. If multiple blocks share the
same exporter identity and `region_id`, MD open fails rather than routing
different peers to the same import mapping. The peer slot index is fixed at 0.

The remainder of this file records superseded explorations and measurements
that led to the current NC-only data path.

Superseded CC-only update as of 2026-06-30: `UCX_OBMM_MEMIDS` may be omitted
when one export is supplied through `UCX_OBMM_SAME_NODE_MEMID`. That mode maps
only the cacheable CC export and does not advertise `INTER_NODE`. NC-only and
mixed NC+CC modes retain their existing behavior; configuring any NC memids
still requires a local NC export.

Superseded status as of 2026-06-30: the former `obmm_nc` and `obmm_cc` TLS are
merged into one `obmm` TLS. `UCX_OBMM_MEMIDS` optionally supplies the local NC
export and mapped NC imports. `UCX_OBMM_SAME_NODE_MEMID` is an optional single
memid; when set, it must resolve to one local export and is mapped cacheable
for same-node AM.

One iface allocates an NC receive FIFO and, when configured, a second receive
FIFO in the same-node export. Its wire address publishes both exporter
identities and both slot indices. Endpoints select the same-node FIFO only when
the peer advertises the exact same same-node exporter identity; otherwise they
use the peer NC identity and the mapped NC import/local export. The wire format
must change because both device and iface addresses change.

UCP sees one AM-only, inter-node-capable TLS and one conservative NC-based
performance model. Same-node CC remains an internal endpoint path with CPU
fences; NC keeps bus-domain fences and explicit LSE shared-control atomics.
Neither path adds zcopy, RMA, atomics, ownership transitions, or libobmm calls.

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

Superseded explicit discovery as of 2026-06-22: the no-list directory-scan
fallback was removed at that point, and the MD required `UCX_OBMM_MEMIDS`.
The 2026-07-06 block-FIFO auto-discovery update restores scanning, but only for
shmdevs whose private metadata exactly matches `ucx-obmm:NN`, avoiding unknown
OBMM regions.

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

Superseded wire-address note: the byte-8 FIFO experiment as of 2026-06-24
regressed large OSU traffic even
after holding `max_short` at 131184. A packed-type A/B produced the same
regression, ruling out removal of `UCS_S_PACKED` as the cause. That layout
therefore restored `length@4`, `header@16`, short byte 16, and the
24-byte element header used by the prior measured baseline. It represents the
2-byte and 8-byte physical gaps with anonymous padding fields, not semantic
reserved members. FIFO element stride and bcopy byte 64 remain unchanged;
physical and advertised `max_short` are both 131184 total AM bytes. The single
TLS address change advanced that format to
`UCT_OBMM_WIRE_FORMAT_PATH_FLAGS` (value 14). The 24-byte device address carries
the primary exporter identity: NC when present, otherwise the sole CC export.
The 56-byte iface address carries explicit path flags, both slot indices, and
the optional same-node identity, keeping both addresses within the default UCP
worker-address v1 limits (31-byte device, 63-byte iface).

Superseded FIFO-depth sizing as of 2026-06-30: the earlier one-export-region
layout used a 4 GiB export to hold a 96-slot, 256-entry FIFO pool. The
2026-07-03 block-FIFO update replaces that with 96 separate export blocks per
node, one slot per block. Receive polling still starts at 64 and grows
adaptively to 128 under sustained pressure. The half-ring maximum publishes
`tail` and dispatches pending sends before a full 256-entry callback batch,
while the larger minimum accelerates slot reclamation for the 384-process
all-to-all target.

UCP sees one logical transport. The UCT endpoint selects the internal path and
the iface progresses all configured receive FIFOs.

## Current Direction

1. Register only `obmm` under the `obmm` component.
2. Auto-discover shmdevs by `priv=ucx-obmm:NN` when `UCX_OBMM_MEMIDS` is
   omitted. Keep `UCX_OBMM_MEMIDS` as an explicit allow-list override.
3. Publish one exporter identity plus `region_id`; the path slot index is
   fixed at 0 in the block-FIFO layout.
4. Select a mapped import or the local export by peer primary identity and
   `region_id`. Never use memid as the peer key.
5. Progress the single receive FIFO from the normal per-iface UCX callback,
   then dispatch pending sends.
6. Preserve UCP protocol-selection logging and one NC-based
   `uct_obmm_iface_estimate_perf()` model.
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

`obmm`:

```text
FIFO_SIZE       = 256
FIFO_ELEM_SIZE  = 131200
BCOPY_SEG_SIZE  = 131072
FIFO_MIN_POLL   = 64
FIFO_MAX_POLL   = 128
BW              = 3400MBs
SHORT_OVERHEAD  = 1800ns
BCOPY_OVERHEAD  = 2us
max_short       = 131184 total AM bytes
max_bcopy       = 131072 bytes
slot_count      = 1 per export block
min_block       = 33,587,392 bytes
block_size      = 34 MiB with 2 MiB OBMM granularity
```

Short and bcopy reuse one FIFO element allocation with overlapping ranges:
short starts at byte 16 and bcopy starts at byte 64.
`BCOPY_SEG_SIZE` is an advertised cap rather than an additive per-entry desc
allocation. This preserves the measured byte-16 short spacing while retaining
64-byte alignment for large bcopy fragments.
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
