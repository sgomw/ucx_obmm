# OBMM Plan: NC-Only Single TLS

Descriptor-backed bcopy update as of 2026-08-25: split inline short storage
from bcopy receive storage following the UCX `mm` ownership pattern. FIFO
elements become 128-byte short/metadata slots; a bcopy element carries a
block-relative offset to one of 512 fixed receive descriptors in the same NC
export block. Bcopy callbacks receive `UCT_CB_PARAM_FLAG_DESC`. On
`UCS_INPROGRESS`, the receiver replaces the FIFO slot's descriptor before
publishing `tail`, and `uct_iface_release_desc()` later returns the retained
buffer to a local free list. Pool exhaustion stalls before consuming the next
bcopy entry and propagates normal FIFO backpressure. Defaults use 64 KiB bcopy
buffers so the complete layout still rounds to the existing 34 MiB block at
2 MiB allocation granularity. `DESIGN.md` is normative for the layout,
ordering, READY publication, and failure boundary.

Design handoff documentation update as of 2026-07-16: `DESIGN.md` now records
the active transport's intent, object/resource ownership, strict discovery
rules, end-to-end lifecycle, shared block/FIFO protocol, NC memory ordering,
pending semantics, failure/recovery boundary, diagnostics, and the intended
future UCT-managed export/import ownership shape. Historical experiments stay
in this plan; future agents should use `DESIGN.md` as the normative active
design and consult this file only for chronology and rejected alternatives.

Same-program job assumption as of 2026-07-09: every MPI launch using obmm is
assumed to start the same UCX/obmm transport program and the same
`UCX_OBMM_*` FIFO geometry on all participating ranks. Mixed binaries,
mixed transport layouts, or mixed geometry are outside UCT obmm's detection
scope and should be reported above UCT. Therefore the active transport should
not carry or validate peer wire-format and FIFO-geometry fields in the UCT
iface address; peer FIFO pointer math uses local iface geometry.

Local controller identity update as of 2026-07-15: ubmem protocol EID width
is at most 20 bits. `/sys/devices/ub_bus_controller*/*/{eid,primary_cna}`
exposes libobmm-style non-negative int-sized scalar controller attributes
from child directories identified by a `ubc` marker file, while OBMM shmdev
`export_info/deid` and `import_info/deid` print EID as `u64 : u64`. In the
current environment the high half of shmdev EID is invalid/unused and must be
zero; UCT validates that form and stores the scalar value as DEID.

FIFO-block simplification as of 2026-07-09: because each shmdev export block
is now occupied by exactly one process/FIFO, the active layout should drop the
old shared allocator, bitmap, per-queue metadata, and queue-index wire field.
A block contains a small FIFO header at region base plus one colored FIFO at
`fifo_offset`. The header carries a 64-bit pid/starttime claim token and the
colored FIFO offset; FIFO geometry remains local configuration. This keeps the code closer to the future
self-export/import shape: one process owns one exported FIFO block, and peers
open that block's FIFO header during EP creation.

Bcopy validation update as of 2026-07-16: the supported scope is the normal UCP
path, which limits pack length from the advertised `cap.am.max_bcopy` before
calling UCT. `am_bcopy` therefore uses the standard `UCT_CHECK_LENGTH()` only
as a parameter-check-build diagnostic and no longer calls `ucs_fatal()` for an
invalid returned length. Official release builds disable this check. OBMM does
not advertise `UCT_IFACE_FLAG_ERRHANDLE_BCOPY_LEN`; a violation after FIFO head
reservation is outside the supported UCP contract and is not a recoverable
FIFO error path. Iface geometry validation still guarantees that the advertised
`bcopy_seg_size` fits in the physical FIFO element bcopy capacity.

Lifecycle refactor update as of 2026-07-08: align the current externally
prepared block-FIFO deployment with the future UCT-managed export/import
shape. The current stage target is to keep the code boundaries close to the
future self export/import flow, while export/import are still prepared outside
UCX. `md_open` now owns only local controller identity; it does not open,
mmap, or keep an allow-list for shmdevs. `iface_init` discovers candidate
local exports by `priv=ucx-obmm:NN`, maps one usable export block, and claims
that block's FIFO header. Reachability resolves the peer tuple through sysfs
without mmaping. `ep_create` resolves and maps only the peer block needed by
that EP, except self-loopback may reuse the iface-owned RX export mapping.
There is no MD-owned import cache, `opened_imports`, or process-local mmap
refcount.

NC-only update as of 2026-07-06: remove the cacheable same-node path and the
internal plane concept from the active transport. `obmm` now has one NC
receive FIFO per iface, one wire address path, and bus-domain fences only.
The same-node memid and memid allow-list configuration knobs are removed.

Auto-discovery update as of 2026-07-06: the active block-FIFO NC deployment
scans `/sys/devices/obmm/obmm_shmdev*`, reads `priv_len`/`priv`, and admits
only mappable shmdevs whose private metadata is exactly `ucx-obmm:NN`. Local
exports among them become claimable FIFO blocks and imports become peer
mappings. There is no active memid allow-list configuration.

FIFO-offset coloring fix as of 2026-07-07, refined on 2026-07-15:
high-concurrency OSU data showed that the block-FIFO layout regressed
small-message latency while `-x` warmup changes and UCP proto selection did
not explain the difference. Target measurements confirmed that avoiding
identical FIFO-control offsets across many independent shmdev blocks restores
the old single-region performance. Hardware follow-up clarified that identical
PA low 9 bits are the sensitive case, so the active FIFO header remains at
region base while `fifo_offset` is selected only to rotate the 64-byte-aligned
FIFO base through the usable low-9-bit values by `region_id`.
FIFO-internal slot coloring is intentionally not changed in this step.

Block-FIFO update as of 2026-07-03: the hardware environment now
pre-provisions 96 NC export shmdev blocks per node, and each process claims one
whole export block instead of allocating from a single large export
region. Remote nodes pre-import each peer node's 96 export blocks, so a two-node
run has 96 imports per node and a four-node run has 96 * 3 imports per node.
Each export/import block contains one FIFO; with the then-current
`FIFO_SIZE=256`, `FIFO_ELEM_SIZE=131200`, and `BCOPY_SEG_SIZE=131072`, the
minimum block footprint is 33,587,392 bytes. Because the OBMM allocation
granularity is 2 MiB, provision each block as 34 MiB.

The sysfs scan may expose multiple local exports and remote imports, but the
MD no longer maps them into a process-wide region table. Each iface claims and
maps one local export block. `region_id` is parsed from `priv=ucx-obmm:NN` so
peers can distinguish multiple export blocks from the same node without using
local memid as the peer key. If multiple visible blocks share the same
exporter identity and `region_id`, peer resolution fails rather than routing
different peers to an ambiguous mapping.

The remainder of this file records superseded explorations and measurements
that led to the current NC-only data path.

Superseded CC-only update as of 2026-06-30: the then-existing explicit NC
memid list could be omitted when one export was supplied through the
same-node memid knob. That mode mapped only the cacheable CC export and did
not advertise `INTER_NODE`. NC-only and mixed NC+CC modes retained their
existing behavior; configuring any NC memids still required a local NC export.

Superseded status as of 2026-06-30: the former `obmm_nc` and `obmm_cc` TLS are
merged into one `obmm` TLS. The old explicit memid list optionally supplied
the local NC export and mapped NC imports. The old same-node memid knob was an
optional single memid; when set, it had to resolve to one local export and was
mapped cacheable for same-node AM.

One iface allocates an NC receive FIFO and, when configured, a second receive
FIFO in the same-node export. Its wire address publishes both exporter
identities and both queue indices. Endpoints select the same-node FIFO only when
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
fallback was removed at that point, and the MD required an explicit memid
list. The 2026-07-06 block-FIFO auto-discovery update restores scanning, but
only for shmdevs whose private metadata exactly matches `ucx-obmm:NN`,
avoiding unknown OBMM regions.

NC-only placement diagnosis as of 2026-06-15: interpret OSU `multi_lat` as
half-split rank pairing, not adjacent-rank pairing. With two 70-rank nodes,
`np=70 map-by-slot` and `np=140 map-by-node` both create 35 same-node NC pairs
per node, and their `obmm_nc` curves are nearly identical across the tested
sizes. This makes the same-node NC large-message regression a per-node local NC
data-path wall rather than a mixed-TL artifact. Use
`ucx/src/uct/obmm/probes/obmm_nc_mem_probe.c` to validate whether the wall is
the local NC mmap read/write bandwidth resource before changing FIFO geometry
or UCP cost defaults.

Superseded allocator/header cleanup simplification as of 2026-06-16: header geometry is
not a hard compatibility gate. On attach, stale metadata from a previous run
is detected by checking the expected metadata area and, when no live owners
exist, warning before clearing the shared region and reinitializing it. Normal
iface cleanup zeroes the owned FIFO bytes; final cleanup zeroes the full
mapped region before publishing UNINIT. Peer open uses the peer iface
address geometry for pointer math instead of validating peer header geometry.
`wire_format` remains the obmm UCT ABI/code guard because OMPI/PML UCX and UCP
worker-address versioning do not prove that both sides loaded the same obmm UCT
code.

Superseded generation-token removal as of 2026-06-22: remove per-allocation
generation tokens from allocator metadata, iface addresses, and FIFO elements.
Allocation now relies on zeroing the complete FIFO bytes before publishing
`IN_USE`; cleanup and final reset also zero FIFO/full-region bytes.

Superseded allocator magic removal as of 2026-06-24: remove the write-only
`magic` word from the shared allocator header. Wire-format exchange cannot protect independently
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
The 56-byte iface address carries explicit path flags, both queue indices, and
the optional same-node identity, keeping both addresses within the default UCP
worker-address v1 limits (31-byte device, 63-byte iface).

Superseded FIFO-depth sizing as of 2026-06-30: the earlier one-export-region
layout used a 4 GiB export to hold a 96-queue, 256-entry FIFO allocator. The
2026-07-03 block-FIFO update replaces that with 96 separate export blocks per
node, one queue per block. Receive polling still starts at 64 and grows
adaptively to 128 under sustained pressure. The half-ring maximum publishes
`tail` and dispatches pending sends before a full 256-entry callback batch,
while the larger minimum accelerates entry reclamation for the 384-process
all-to-all target.

UCP sees one logical transport. The UCT endpoint selects the internal path and
the iface progresses all configured receive FIFOs.

## Current Direction

Lifecycle cleanup refinement (2026-07-23): follow the UCX `mm` class pattern.
An iface constructor releases any obmm resource it acquired before returning a
failure. Its class cleanup is reserved for successfully constructed ifaces,
because UCX only invokes cleanup for parent classes whose constructors
completed after a derived-class init failure. This removes redundant
`base_initialized` and `arbiter_initialized` state from the iface; no FIFO,
address, capability, or UCP protocol behavior changes.

1. Register only `obmm` under the `obmm` component.
2. Auto-discover shmdevs only by `priv=ucx-obmm:NN`; do not expose a memid
   allow-list.
3. Publish one exporter identity plus `region_id`; the current iface address
   is empty (`iface_addr_len = 0`).
4. Resolve and map the peer block at EP creation by peer primary identity and
   `region_id`. Never use memid as the peer key.
5. Progress the single receive FIFO from the normal per-iface UCX callback,
   then dispatch pending sends.
6. Preserve UCP protocol-selection logging and one NC-based
   `uct_obmm_iface_estimate_perf()` model.
7. Keep short inline in a 128-byte FIFO element and place bcopy data in the
   fixed receive-descriptor pool. Advertise 64 KiB bcopy fragments initially;
   re-measure protocol selection and large-message behavior on target because
   the previous 128 KiB measurement applied to the superseded inline layout.

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
FIFO_ELEM_SIZE  = 128
BCOPY_SEG_SIZE  = 65536
RX_DESC_COUNT   = 512
FIFO_MIN_POLL   = 64
FIFO_MAX_POLL   = 128
BW              = 3400MBs
SHORT_OVERHEAD  = 1800ns
BCOPY_OVERHEAD  = 2us
max_short       = 112 total AM bytes
max_bcopy       = 65536 bytes
block_layout    = one small FIFO plus one fixed bcopy descriptor pool
min_block       = about 33,620,216 bytes at zero rx headroom/default alignment
block_size      = 34 MiB with 2 MiB OBMM granularity
```

Short starts at byte 16 in the FIFO element. The persistent byte-8 field is a
block-relative offset to an aligned bcopy descriptor payload; short sends do
not overwrite it. The exact minimum block size includes UCT rx
headroom/alignment and is validated at iface open.
Prefer 64-byte-aligned FIFO element, bcopy segment, and descriptor strides
unless new measurements prove otherwise.

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
