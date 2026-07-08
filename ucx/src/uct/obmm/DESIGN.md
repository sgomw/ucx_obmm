# OBMM UCT Transport Design

Status: the accepted transport is AM-only and NC-only. It exposes one logical
UCT TLS:

```text
obmm: NC AM over pre-exported OBMM shmdev blocks
```

UCP sees one capability and performance model. Each iface owns one NC receive
FIFO by claiming one local export block. Endpoints match peers by exporter
identity plus `region_id` and use either the mapped import for a remote export
or the local export for self-loopback. There is no secondary local-memory path.

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
- The transport discovers shmdevs through sysfs and maps `/dev/obmm_shmdev*`
  directly.
- `UCX_OBMM_MEMIDS` is optional. When omitted, the transport scans
  `/sys/devices/obmm/obmm_shmdev*` and maps every mappable shmdev whose private
  metadata is exactly `ucx-obmm:NN`. This is the normal block-FIFO deployment
  mode.
- When set, `UCX_OBMM_MEMIDS` must contain one or more local exports plus the
  imports needed for remote peers, and disables the automatic scan. The current
  target provisions one local export block per process; each process claims one
  whole export block. In this explicit mode, empty private metadata is accepted
  as the legacy no-id case, but non-empty private metadata must still use
  `ucx-obmm:NN`.
- Any mapped NC import can supply the local DCNA needed to identify local
  exports.
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

## Capabilities

`obmm` advertises:

| Capability | Status | Notes |
| --- | --- | --- |
| `AM_SHORT` | yes | FIFO inline payload |
| `AM_BCOPY` | yes | shared-data FIFO fragment path |
| `PENDING` | yes | arbiter-backed retry on FIFO backpressure |
| `CONNECT_TO_IFACE` | yes | endpoint uses peer device and iface addresses |
| `CB_SYNC` | yes | callback data is valid only during callback |
| `INTER_NODE` | yes | Advertised when the iface owns its NC RX FIFO |

The ops table also supports `AM_SHORT_IOV` through the UCX base helper, which
packs the iov into the existing FIFO-backed `AM_SHORT` operation. This does not
add a separate capability flag or zero-copy data path.

The internal ops provide diagnostics only: VFS refresh exposes local obmm FIFO
and pool state, while endpoint query succeeds only for an empty field mask and
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

## Pool Geometry

The current hardware environment pre-provisions one shmdev export block per
process, so process fanout comes from multiple local exports rather than
multiple slots inside one large export.

```text
slot_stride = fifo_control + FIFO_SIZE * FIFO_ELEM_SIZE
required    = colored_slot_offset + slot_count * slot_stride
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
slot_count      = 1
short_capacity  = 131184 total AM bytes
max_short       = 131184 total AM bytes
max_bcopy       = 131072 bytes
```

The default geometry has a minimum footprint of 33,587,392 bytes, or
32.031 MiB, in each configured export block. With the current 2 MiB OBMM
allocation granularity, each export block should be provisioned as 34 MiB. A
96-process node therefore uses 96 local export blocks, for 3264 MiB of local
NC export capacity. One `UCX_OBMM_*` geometry configuration applies to the
receive FIFO.

As of the 2026-07-07 small-message regression fix, slot 0 is no longer placed
at the same block-relative offset in every export/import block. The pool
header, bitmap, and slot metadata remain fixed at the region base, but
`slot_array_offset` is chosen from the 34 MiB block's spare space. The
`region_id` itself remains the parsed `ucx-obmm:NN` identity; the color offset
is derived separately as `NN * color_step`, rounded to 64 bytes. `color_step`
is the FIFO slot stride modulo the 2 MiB OBMM allocation granule, matching the
natural offset progression of the old single-region layout. With the default
34 MiB block this gives up to 2,064,192 bytes of offset slack. Target OSU
measurements showed this restored the old single-region small-message
performance by avoiding identical FIFO-control offsets across many independent
shmdev blocks.

Receive polling starts at 64 completions and adaptively grows to 128 when
successive progress calls consume the complete poll window. A low-traffic call
still stops at the first unpublished FIFO element. The 128 upper bound drains
at most half of the 256-entry ring before publishing `tail` and dispatching
pending sends, avoiding a full-ring callback batch that would delay both.

Prefer 64-byte-aligned `FIFO_ELEM_SIZE` and `BCOPY_SEG_SIZE` unless new target
measurements prove otherwise. Non-64B-aligned strides have regressed latency on
the current platform.

---

## Wire Format

The active wire format is `UCT_OBMM_WIRE_FORMAT_NC_ONLY` (value 18). FIFO
elements retain `length@4`, the short header at byte 16, bcopy at byte 64, and
anonymous physical padding. The wire value changes because the pool slot
offset is region-colored and `region_id` now carries the parsed
`ucx-obmm:NN` value rather than a CRC32 of the private metadata. Older builds
assume either a fixed slot offset or the former CRC32 region-id semantics. All
processes that attach the same local export block must use this build.

`uct_obmm_device_addr_t` carries:

```text
primary exporter identity
```

`uct_obmm_iface_addr_t` carries:

```text
slot_index, pid, wire_format, fifo_size, fifo_elem_size, bcopy_seg_size
```

The claimed receive slot is index 0 in the current one-slot export-block
layout. Region addresses include exporter DCNA/DEID plus a 32-bit `region_id`
parsed from shmdev `priv=ucx-obmm:NN` metadata. A region without transport
private metadata uses `UINT32_MAX` as an internal no-id value and is accepted
only when the mapped regions remain unambiguous. The device address is 28
bytes and the iface address is 24 bytes, fitting worker-address v1's
respective 31-byte and 63-byte limits. `wire_format` remains the obmm UCT ABI/code guard. FIFO
geometry is the peer runtime layout used for pointer math; `ep_create`
validates geometry and slot 0 against its region.

---

## Reachability

The local iface scans the configured local exports and claims the first free
export block.

For each peer:

1. Match the peer primary exporter identity plus `region_id` against a mapped
   import or the local export and select the peer slot 0.
2. If no matching region exists, the peer is unreachable.

Memid is never used as the peer key, so local memids may be in a different
order from the remote node's imports as long as the exported `priv` metadata
produces the same `region_id`.

---

## Send Path

`am_short` reserves one peer FIFO slot with explicit LSE CAS on peer `head`.
It writes the FIFO element, copies payload inline after the AM header field,
issues a bus-domain release fence, and publishes the owner bit.

`am_bcopy` reserves one peer FIFO slot, writes the packed payload into the
same shared FIFO data area used by short, issues the same bus-domain release
fence, and publishes a FIFO element with the bcopy flag.

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

Claiming an export block zeroes its single FIFO slot before publishing the
metadata as `IN_USE`; no per-slot generation token is carried in the iface
address or FIFO element. On normal iface cleanup, the owned FIFO slot is also
zeroed before it is released. When the block's single slot is released, pool
reset keeps the state in INITING while zeroing the full mapped block, then
publishes UNINIT. If a prior run left READY metadata but no live owner, the
next attach warns, clears the shared block, and reinitializes it. A hard
process death cannot execute UCX cleanup at the instant of failure; stale data
from that case is cleared by a later claimant that can prove the owner is dead,
or by the next attach/reinitialization path if the whole job is gone.

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

MD-level region classification knobs:

| Config | Meaning |
| --- | --- |
| `UCX_OBMM_MEMIDS` | optional shmdev allow-list; when omitted, auto-scan all `priv=ucx-obmm:NN` shmdevs |

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
3. `ucx_info -c | grep OBMM` should show the shared `OBMM_*` tuning knobs and
   `OBMM_MEMIDS`, with no same-node or `OBMM_CC_*` configuration.
4. Validate NC-only auto-discovery and explicit `UCX_OBMM_MEMIDS` cases.
5. Validate OSU behavior on the real setup; do not claim target performance
   from local static checks.
