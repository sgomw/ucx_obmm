# OBMM UCT Transport Design

Status: the accepted transport is AM-only and topology-aware. It exposes one
logical UCT TLS:

```text
obmm: NC AM with an optional cacheable same-node path
```

UCP sees one capability and performance model. The endpoint internally selects
NC or same-node CC from the exchanged exporter identities. An iface owns an NC
receive FIFO, a same-node CC receive FIFO, or both. CC-only mode is local-only;
NC remains mandatory whenever cross-node operation is configured.

Cross-node cacheable CC as a UCT data path was explored and rejected on
2026-06-05. Do not implement or tune staged CC zcopy, sender-owned CC,
receiver-owned CC, or CC batch/epoch cross-node paths unless a new design is
explicitly approved. Same-node CC direct AM is not part of that rejected route:
it uses local cacheable shared memory and no ownership transitions.

---

## Environment Facts

- Export/import is done outside UCX. UCT must not call `obmm_export`,
  `obmm_unexport`, `obmm_import`, `obmm_unimport`, `obmm_preimport`, or
  `obmm_unpreimport`.
- The transport discovers shmdevs through sysfs and maps `/dev/obmm_shmdev*`
  directly.
- At least one of `UCX_OBMM_MEMIDS` or `UCX_OBMM_SAME_NODE_MEMID` is required.
- When set, `UCX_OBMM_MEMIDS` must contain exactly one local NC export plus the
  imports needed for remote peers.
- `UCX_OBMM_SAME_NODE_MEMID` accepts exactly one memid and sysfs must identify
  that shmdev as an export. It is mapped cacheable and is never used for
  cross-node access. When it is the only configured option, the iface operates
  in local-only CC mode.
- NC and optional same-node memids are discovered in one sysfs pass and then
  classified. Any mapped NC import can supply the local DCNA needed to
  identify both exports.
- Sysfs discovery accepts only an explicit non-empty memid list; it never
  scans all shmdev directories.
- NC mappings are opened as `open(..., O_RDWR | O_SYNC)` and mapped with
  `MAP_SHARED | PROT_READ | PROT_WRITE`.
- Same-node CC mappings intentionally omit `O_SYNC` so cacheable local shared
  memory remains cacheable.
- Do not call `obmm_set_ownership()` from the UCT transport. It is irrelevant
  for NC and not needed for same-node CC direct AM.
- Peer matching is by exporter identity, not memid. Use exporter DCNA/DEID
  from sysfs `export_info`/`import_info`.
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
| `INTER_NODE` | conditional | Advertised only when the iface owns an NC RX FIFO |

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

Both internal paths use the same FIFO/pool layout:

```text
slot_stride = fifo_control + FIFO_SIZE * FIFO_ELEM_SIZE
required    = pool_header + slot_count * slot_stride
```

`FIFO_ELEM_SIZE` contains the FIFO metadata plus overlapping short and bcopy
data ranges in one allocation. `am_short` stores `[header | payload]` starting
at byte 16 after an isolated FIFO metadata prefix. `am_bcopy` stores the
packed payload starting at byte 64 because target measurements require aligned
large-fragment writes. A FIFO element carries only one AM type, so the ranges
may overlap without allocating a second per-entry desc array.

Current defaults for both internal paths:

```text
FIFO_SIZE       = 256
FIFO_ELEM_SIZE  = 131200
BCOPY_SEG_SIZE  = 131072
FIFO_MIN_POLL   = 64
FIFO_MAX_POLL   = 128
slot_count      = 96
short_capacity  = 131184 total AM bytes
max_short       = 131184 total AM bytes
max_bcopy       = 131072 bytes
```

The default geometry requires 3,224,385,856 bytes, or 3075.014 MiB, in each
configured export. It fits in a 4 GiB region with 1,070,581,440 bytes
(1020.986 MiB) left for region-level headroom. One `UCX_OBMM_*` geometry
configuration applies to both receive FIFOs.

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

The active wire format is `UCT_OBMM_WIRE_FORMAT_PATH_FLAGS` (value 14). FIFO
elements retain `length@4`, the short header at byte 16, bcopy at byte 64, and
anonymous physical padding. The wire value changes because addresses now carry
explicit NC/same-node path flags and support an iface with no NC RX path. All
processes that attach the same local export region must use this build.

`uct_obmm_device_addr_t` carries:

```text
primary exporter identity: NC when present, otherwise the sole same-node CC
```

`uct_obmm_iface_addr_t` carries:

```text
optional same-node exporter identity, nc_slot_index, same_node_slot_index,
pid, wire_format, path_flags,
fifo_size, fifo_elem_size, bcopy_seg_size
```

An absent path has its slot index set to `UINT32_MAX` and its path flag clear;
an absent same-node path also has a zero identity. The device address is 24
bytes and the iface address is 56 bytes, fitting worker-address v1's respective
31-byte and 63-byte limits. `wire_format` remains the obmm UCT ABI/code guard.
FIFO geometry is the peer runtime layout used for pointer math; `ep_create`
validates path flags, geometry, and the selected slot against its region.

---

## Reachability

The local iface attaches every configured local export. It may have only NC,
only same-node CC, or both receive paths.

For each peer:

1. If both sides advertise a same-node slot and the peer same-node exporter
   identity exactly matches the local same-node export, select that cacheable
   region and slot.
2. Otherwise, if both peers advertise NC, match the peer primary exporter
   identity against a mapped NC import or the local NC export and select the
   peer NC slot.
3. If neither region exists, the peer is unreachable.

This makes different or missing same-node configuration fall back to NC only
when NC exists on both sides, rather than interpreting a slot index in the
wrong export. A CC-only peer is unreachable from an iface without the matching
same-node export. Memid is never used as the peer key.

---

## Send Path

`am_short` reserves one peer FIFO slot with explicit LSE CAS on peer `head`.
It writes the FIFO element, copies payload inline after the AM header field,
issues a plane-specific release fence, and publishes the owner bit.

`am_bcopy` reserves one peer FIFO slot, writes the packed payload into the
same shared FIFO data area used by short, issues the same plane-specific
release fence, and publishes a FIFO element with the bcopy flag.

Path fences:

- NC uses bus-domain fences because cross-host non-cacheable memory visibility
  depends on bus ordering.
- CC uses CPU fences because the path is same-node cacheable shared memory.

If the peer FIFO is full, send returns `UCS_ERR_NO_RESOURCE`. `pending_add`
queues requests on an iface arbiter and preserves FIFO order by not returning
`UCS_ERR_BUSY` when older queued requests exist for that endpoint.

`ep_flush` and `iface_flush` complete immediately because the AM path has no
asynchronous operation once a FIFO element has been published.

---

## Receive And Progress

The per-iface receive path polls each active local FIFO until its per-call
budget is exhausted or the next expected owner bit is absent. After observing
a published element, the receiver issues the matching path-specific acquire
fence before reading payload fields.

Inline short payload invokes the AM callback from the FIFO element. Bcopy
payload invokes the AM callback from the same FIFO element data area. Callback
data is valid for callback lifetime only.

OBMM uses one UCX per-iface progress callback. It always polls NC and, when the
same-node export is configured, also polls the CC FIFO. Poll order alternates
between calls. Pending dispatch runs after each active receive path so merging
the TLS does not halve retry opportunities under mixed traffic.

Slot allocation zeroes the complete slot before publishing the metadata as
`IN_USE`; no per-slot generation token is carried in the iface address or FIFO
element. On normal iface cleanup, the owned FIFO slot is also zeroed before it
is released. When the final local slot is released, pool reset keeps the state
in INITING while zeroing the full mapped region, then publishes UNINIT. If a
prior run left READY metadata but no live owners, the next attach warns, clears
the shared region, and reinitializes it. A hard process death cannot execute
UCX cleanup at the instant of failure; stale data from that case is cleared by
a later final cleanup that can prove the slot owner is dead, or by the next
attach/reinitialization path if the whole job is gone.

---

## UCP Tuning Hooks

`uct_obmm_iface_estimate_perf()` is retained for the single TLS.
It reports:

- `UCT_EP_OP_AM_SHORT`: configured bandwidth plus `SHORT_OVERHEAD`.
- `UCT_EP_OP_AM_BCOPY`: configured bandwidth plus `BCOPY_OVERHEAD`.
- unsupported operations: `UCS_ERR_UNSUPPORTED`.

Default tuning uses the `UCX_OBMM_*` prefix: `3400MBs` bandwidth, `1800ns`
short overhead, and `2us` bcopy overhead. These conservative NC values model
the mandatory cross-node path. UCP does not receive a separate same-node cost;
the endpoint selects CC internally.

MD-level region classification knobs:

| Config | Meaning |
| --- | --- |
| `UCX_OBMM_MEMIDS` | optional NC shmdev list; required for cross-node mode |
| `UCX_OBMM_SAME_NODE_MEMID` | optional single same-node export; may be the sole local-only path |

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
2. In NC or mixed mode, `UCX_TLS=obmm ucx_info -d -t obmm` should show one TLS
   with AM short/bcopy, pending, and `INTER_NODE`, with no `am_zcopy`.
3. In CC-only mode, the same command should show AM short/bcopy and pending but
   must not show `INTER_NODE`.
4. `ucx_info -c | grep OBMM` should show the shared `OBMM_*` tuning knobs,
   `OBMM_MEMIDS`, and `OBMM_SAME_NODE_MEMID`, with no `OBMM_NC_*`/
   `OBMM_CC_*` iface tuning groups or `OBMM_CC_MEMIDS`.
5. Validate NC-only, CC-only, and mixed configuration cases.
6. Validate OSU behavior on the real setup; do not claim target performance
   from local static checks.
