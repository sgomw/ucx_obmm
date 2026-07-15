# OBMM UCT Transport Design

Status: the accepted transport is AM-only and NC-only. It exposes one logical
UCT TLS:

```text
obmm: NC AM over pre-exported OBMM shmdev blocks
```

UCP sees one capability and performance model. Each iface owns one NC receive
FIFO by claiming one local export block. Endpoints match peers by exporter
identity plus `region_id`; EP setup locates and maps the peer block it will
actually use. There is no secondary local-memory path.

Cross-node cacheable CC as a UCT data path was explored and rejected on
2026-06-05. Do not implement or tune staged CC zcopy, sender-owned CC,
receiver-owned CC, or CC batch/epoch cross-node paths unless a new design is
explicitly approved. Same-node cacheable direct AM was also removed from the
active transport on 2026-07-06; the supported runtime path is NC only.

---

## Environment Facts

- Export/import is done outside UCX. UCT must not call `obmm_export`,
  `obmm_unexport`, `obmm_import`, `obmm_unimport`, `obmm_preimport`, or
  `obmm_unpreimport`.
- A single MPI job is assumed to start the same UCX/obmm transport program
  and the same `UCX_OBMM_*` geometry configuration on every participating
  rank. Mixed UCX binaries, mixed obmm transport layouts, or mixed FIFO
  geometry inside one job are outside this UCT transport's responsibility; if
  such a launch is attempted, detection and reporting belong above UCT.
- The transport discovers OBMM identity and shmdev metadata through sysfs and
  maps `/dev/obmm_shmdev*` directly only at the component that needs the block.
- Local UB controller identity is discovered under
  `/sys/devices/ub_bus_controller*`. Controller attribute directories are
  children such as `00001` and are identified by a `ubc` marker file, matching
  libobmm's `*/ubc` anchored layout. Their `eid` and `primary_cna` files are
  libobmm-style non-negative int-sized scalar values that may be decimal or
  `0x`-prefixed hexadecimal. The ubmem EID protocol width is at most 20 bits,
  so UCT stores controller EID as a scalar value. OBMM shmdev
  `export_info/deid` and
  `import_info/deid` print EID as `u64 : u64`; in this environment the high
  half is invalid/unused and must be zero. UCT validates that form and stores
  the scalar value as DEID.
- The current discovery and mapping lifecycle is defined in
  "Discovery And Mapping Lifecycle" below.
- NC mappings are opened as `open(..., O_RDWR | O_SYNC)` and mapped with
  `MAP_SHARED | PROT_READ | PROT_WRITE`.
- Do not call `obmm_set_ownership()` from the UCT transport. It is irrelevant
  for NC.
- Peer matching is by exporter identity, not memid. Use exporter DCNA/DEID
  from sysfs `export_info`/`import_info` plus the transport `region_id`
  parsed from shmdev `priv=ucx-obmm:NN` metadata when multiple blocks share an
  exporter.
- On arm64 NC mappings, shared control-word atomic RMW must use explicit LSE
  instructions. Do not rely on compiler-lowered LL/SC atomics or generic
  `ucs_atomic_*` for shared NC control words.

---

## Discovery And Mapping Lifecycle

The current stage target is to move the externally prepared deployment toward
the future UCT-managed export/import shape: one process exports/maps its own
RX block, publishes the exporter tuple, and peers resolve/import/map only the
tuple needed for a connection. While export/import are still prepared outside
UCX, the active transport uses private metadata discovery only; there is no
user-supplied memid allow-list.

The ownership boundary is:

1. `md_open` discovers only local controller identity and lightweight sysfs
   access helpers. It does not mmap shmdevs, does not enumerate a process-wide
   region table for later endpoint lookup, and does not own import mappings.
2. `iface_init` finds candidate local export shmdevs by transport private
   metadata, maps exactly one usable export block, initializes/claims that
   block's FIFO header, and owns this RX export mapping until iface cleanup.
3. `iface_is_reachable_v2` checks that the peer exporter tuple can be resolved
   through sysfs. It must not mmap the peer block and must not depend on an MD
   pre-mapped region list.
4. `ep_create` resolves the peer-published `(dcna, eid, region_id)` tuple to
   the local shmdev that represents the peer FIFO, then maps that one block and
   opens the peer FIFO header. For remote peers this is an import shmdev; for
   same-node peers it may be another local export shmdev; for self-loopback it
   may reuse the iface-owned export mapping.
5. EP cleanup releases only mappings that the EP created. It must not unmap
   the iface-owned RX export mapping.

There is intentionally no MD-owned `opened_imports` table, import mmap cache,
or process-local mmap refcount in this design. A UCT process is allowed to map
the peer block it needs at EP creation time and release that EP-owned mapping
when the EP is destroyed. Cross-process mmap state is outside UCT's address
space and cannot be represented by a process-local refcount.

This lifecycle intentionally keeps discovery and mapping at the same component
boundaries that future UCT-managed export/import will use.

---

## Capabilities

`obmm` advertises:

| Capability | Status | Notes |
| --- | --- | --- |
| `AM_SHORT` | yes | FIFO inline payload |
| `AM_BCOPY` | yes | shared-data FIFO fragment path |
| `PENDING` | yes | arbiter-backed retry on FIFO backpressure |
| `CONNECT_TO_IFACE` | yes | endpoint uses peer device address; iface address is empty |
| `CB_SYNC` | yes | callback data is valid only during callback |
| `INTER_NODE` | yes | Advertised when the iface owns its NC RX FIFO |

The ops table also supports `AM_SHORT_IOV` through the UCX base helper, which
packs the iov into the existing FIFO-backed `AM_SHORT` operation. This does not
add a separate capability flag or zero-copy data path.

The internal ops provide diagnostics only: VFS refresh exposes local obmm FIFO
block state, while endpoint query succeeds only for an empty field mask and
returns unsupported for sockaddr fields because obmm endpoints do not have
socket addresses.

Not advertised: `AM_ZCOPY`, PUT/GET/RMA, atomics, `EP_CHECK`,
AM_DUP, and ERRHANDLE_PEER. The ops table must keep unsupported stubs for
unsupported entries.

The MD also does not support UCX user-buffer memory management or remote-key
semantics: `mem_alloc`, `mem_free`, `mem_advise`, `mem_reg`, `mem_dereg`,
`mem_query`, `mem_attach`, `detect_memory_type`, `mkey_pack`, component
`rkey_unpack`, `rkey_ptr`, and `rkey_release` all return unsupported. OBMM only
maps the externally prepared transport FIFO regions; it never registers or
packs user send/receive buffers.

---

## FIFO Block Layout

The current hardware environment pre-provisions one shmdev export block per
process, so process fanout comes from multiple local exports rather than from
sub-allocating one large export. Each block contains one FIFO. There is no
shared allocator, bitmap, per-FIFO allocation metadata, or FIFO index in the
active layout.

```text
fifo_stride = fifo_control + FIFO_SIZE * FIFO_ELEM_SIZE
required    = colored_fifo_offset + fifo_stride
```

`FIFO_ELEM_SIZE` contains the FIFO metadata plus overlapping short and bcopy
data ranges in one allocation. `am_short` stores `[header | payload]` starting
at byte 16 after an isolated FIFO metadata prefix. `am_bcopy` stores the
packed payload starting at byte 64 because target measurements require aligned
large-fragment writes. A FIFO element carries only one AM type, so the ranges
may overlap without allocating a second per-entry desc array.

Current defaults:

```text
FIFO_SIZE       = 256
FIFO_ELEM_SIZE  = 131200
BCOPY_SEG_SIZE  = 131072
FIFO_MIN_POLL   = 64
FIFO_MAX_POLL   = 128
short_capacity  = 131184 total AM bytes
max_short       = 131184 total AM bytes
max_bcopy       = 131072 bytes
```

The default geometry has a minimum footprint of 33,587,392 bytes before
coloring slack, in each configured export block. With the current 2 MiB OBMM
allocation granularity, each export block should be provisioned as 34 MiB. A
96-process node therefore uses 96 local export blocks, for 3264 MiB of local
NC export capacity. One `UCX_OBMM_*` geometry configuration applies to the
receive FIFO.

As of the 2026-07-07 small-message regression fix, the FIFO is no longer
placed at the same block-relative offset in every export/import block. The
FIFO block header remains fixed at the region base, but `fifo_offset` is
chosen from the 34 MiB block's spare space. The `region_id` itself remains the
parsed `ucx-obmm:NN` identity; the color offset is derived separately as
`NN * color_step`, rounded to 64 bytes. `color_step` is the FIFO stride modulo
the 2 MiB OBMM allocation granule, matching the natural offset progression of
the old single-region layout. Target OSU measurements showed this restored the
old single-region small-message performance by avoiding identical FIFO-control
offsets across many independent shmdev blocks.

Receive polling starts at 64 completions and adaptively grows to 128 when
successive progress calls consume the complete poll window. A low-traffic call
still stops at the first unpublished FIFO element. The 128 upper bound drains
at most half of the 256-entry ring before publishing `tail` and dispatching
pending sends, avoiding a full-ring callback batch that would delay both.

Prefer 64-byte-aligned `FIFO_ELEM_SIZE` and `BCOPY_SEG_SIZE` unless new target
measurements prove otherwise. Non-64B-aligned strides have regressed latency on
the current platform.

---

## Address And FIFO Format

The transport no longer carries a peer ABI or FIFO-geometry guard in the UCT
iface address. The job-level same-UCX assumption above is the compatibility
contract. FIFO elements retain `length@4`, the short header at byte 16, bcopy
at byte 64, and anonymous physical padding.

`uct_obmm_device_addr_t` carries:

```text
primary exporter identity
```

`uct_obmm_iface_addr_t` carries:

```text
no bytes; iface_addr_len is 0
```

Region addresses include exporter DCNA, exporter DEID, plus a 32-bit
`region_id` parsed from shmdev `priv=ucx-obmm:NN` metadata. Regions without
transport private metadata are not transport candidates. The device address is
16 bytes and the iface address is 0 bytes, fitting worker-address v1's limits.
`ep_create`
uses the local iface geometry for peer FIFO pointer math because the peer is
assumed to run the same transport code and configuration in the same MPI job.

---

## Reachability

MD provides local identity only. The local iface owns one claimed export block.

For each peer:

1. Resolve the peer primary exporter identity plus `region_id` through sysfs
   to the local shmdev representing the peer FIFO.
2. `iface_is_reachable_v2` performs only this resolution check; it
   does not mmap the peer block.
3. `ep_create` maps the resolved block as described in
   "Discovery And Mapping Lifecycle" and opens the peer FIFO header.
4. If no matching shmdev exists, the peer is unreachable.

Memid is never used as the peer key, so local memids may be in a different
order from the remote node's imports as long as the exported `priv` metadata
produces the same `region_id`.

---

## Send Path

`am_short` reserves one peer FIFO element with explicit LSE CAS on peer `head`.
It writes the FIFO element, copies payload inline after the AM header field,
issues a bus-domain release fence, and publishes the owner bit.

`am_bcopy` reserves one peer FIFO element, writes the packed payload into the
same shared FIFO data area used by short, hard-validates the `pack_cb` returned
length against the advertised `BCOPY_SEG_SIZE`, issues the same bus-domain
release fence, and publishes a FIFO element with the bcopy flag. If
`BCOPY_SEG_SIZE` does not fit in the physical FIFO element bcopy capacity,
iface open fails during geometry validation, so the send hot path only checks
the advertised cap. Invalid bcopy lengths are fatal because the payload has
already been written into the shared FIFO slot and continuing could publish a
corrupted element.

All fences are NC bus-domain fences because cross-host non-cacheable memory
visibility depends on bus ordering. `ep_fence` and `iface_fence` apply the
full bus-domain fence.

If the peer FIFO is full, send returns `UCS_ERR_NO_RESOURCE`. `pending_add`
queues requests on an iface arbiter and preserves FIFO order by not returning
`UCS_ERR_BUSY` when older queued requests exist for that endpoint.

`ep_flush` and `iface_flush` complete immediately because the AM path has no
asynchronous operation once a FIFO element has been published.

---

## Receive And Progress

The per-iface receive path polls its local FIFO until its per-call budget is
exhausted or the next expected owner bit is absent. After observing a
published element, the receiver issues the matching bus-domain acquire fence
before reading payload fields.

Inline short payload invokes the AM callback from the FIFO element. Bcopy
payload invokes the AM callback from the same FIFO element data area. Callback
data is valid for callback lifetime only.

OBMM uses one UCX per-iface progress callback. It polls the single NC FIFO and
then dispatches pending sends.

Claiming an export block uses one 64-bit shared `claim` word. `claim=0` means
free; a nonzero pid/starttime token means initialization is in progress; the
same token with the READY bit set means the FIFO header is ready. The claim
CAS therefore publishes the initializer identity atomically, before any FIFO
bytes are cleared or initialized. On normal iface cleanup, the owner CASes its
READY claim back to the non-ready token, clears the mapped block while keeping
that claim, then publishes `claim=0`. If a prior run left READY or in-progress
metadata but the encoded owner is no longer live, the next attach warns,
claims the block, clears the shared block, and reinitializes it. A live
nonzero claim is treated as occupied; attach does not wait for another live
process that is still initializing.

---

## UCP Tuning Hooks

`uct_obmm_iface_estimate_perf()` is retained for the single TLS.
It reports:

- `UCT_EP_OP_AM_SHORT`: configured bandwidth plus `SHORT_OVERHEAD`.
- `UCT_EP_OP_AM_BCOPY`: configured bandwidth plus `BCOPY_OVERHEAD`.
- unsupported operations: `UCS_ERR_UNSUPPORTED`.

Default tuning uses the `UCX_OBMM_*` prefix: `3400MBs` bandwidth, `1800ns`
short overhead, and `2us` bcopy overhead. These conservative NC values model
the NC path.

There are no MD-level region classification knobs. Candidate transport blocks
are discovered by `priv=ucx-obmm:NN` metadata, and memid is never a peer
identity or user-facing allow-list.

UCP protocol-selection logging is intentionally retained. Use
`UCX_PROTO_SELECT_LOG=y` and `UCX_PROTO_SELECT_LOG_RANK=<rank>` to emit
one-shot `ucp_proto_select:` lines for selected UCT lanes without mixing with broad
debug logs.

---

## Verification

Local Windows verification is limited to static checks. Do not run `mpirun`,
`ucx_perftest`, or two-node hardware tests locally.

Target checks:

1. Build UCX on Linux.
2. `UCX_TLS=obmm ucx_info -d -t obmm` should show one TLS with AM short/bcopy,
   pending, and `INTER_NODE`, with no `am_zcopy`.
3. `ucx_info -c | grep OBMM` should show the shared `OBMM_*` tuning knobs,
   with no memid allow-list, same-node, or `OBMM_CC_*` configuration.
4. Validate NC-only local identity discovery, iface export claiming, and
   ep-time peer block resolution.
5. Validate OSU behavior on the real setup; do not claim target performance
   from local static checks.
