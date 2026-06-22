# OBMM UCT Transport Design

Status: the accepted transport is AM-only and topology-aware. It exposes two
logical UCT TLS under one `obmm` component:

```text
obmm_nc: cross-node AM over non-cacheable NC memory
obmm_cc: same-node AM over cacheable CC memory
```

The two TLS keep UCP performance and reachability decisions separate, while
using the normal UCX per-iface progress path for each active OBMM iface. This
avoids the mixed-performance problem of one monolithic TLS without carrying a
private shared worker-progress layer. A 2026-06-15 target comparison found the
former shared OBMM worker callback and the simpler per-iface callback path
performed essentially the same, so the simpler path is preferred.

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
- The hardware does not expose whether a shmdev is NC or CC to this transport.
  Users classify regions with `UCX_OBMM_NC_MEMIDS` and `UCX_OBMM_CC_MEMIDS`.
  `CC_MEMIDS` may be provided alone for same-node-only `obmm_cc`; `obmm_nc`
  uses `NC_MEMIDS`. If neither list is configured, the MD reports no device.
- Explicit NC/CC memids are discovered in one sysfs pass and then classified.
  Any mapped import can supply the local DCNA needed to identify exports, so
  same-node CC does not require a remote CC import for reachability.
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

`obmm_nc` advertises:

| Capability | Status | Notes |
| --- | --- | --- |
| `AM_SHORT` | yes | NC FIFO inline payload |
| `AM_BCOPY` | yes | NC shared-data FIFO fragment path |
| `PENDING` | yes | arbiter-backed retry on FIFO backpressure |
| `CONNECT_TO_IFACE` | yes | endpoint uses peer device and iface addresses |
| `CB_SYNC` | yes | callback data is valid only during callback |
| `INTER_NODE` | yes | NC is the cross-node plane |

`obmm_cc` advertises the same AM/pending/connect capabilities but does not
advertise `INTER_NODE`. Its reachability accepts only peers whose device
address names the same local CC export region.

The ops table also supports `AM_SHORT_IOV` through the UCX base helper, which
packs the iov into the existing FIFO-backed `AM_SHORT` operation. This does not
add a separate capability flag or zero-copy data path.

The internal ops provide diagnostics only: VFS refresh exposes local obmm FIFO
and pool state, while endpoint query succeeds only for an empty field mask and
returns unsupported for sockaddr fields because obmm endpoints do not have
socket addresses.

Not advertised by either plane: `AM_ZCOPY`, PUT/GET/RMA, atomics, `EP_CHECK`,
AM_DUP, and ERRHANDLE_PEER. The ops table must keep unsupported stubs for
unsupported entries.

---

## Pool Geometry

Both planes use the same FIFO/pool layout:

```text
slot_stride = fifo_control + FIFO_SIZE * FIFO_ELEM_SIZE
required    = pool_header + slot_count * slot_stride
```

`FIFO_ELEM_SIZE` contains the FIFO metadata plus overlapping short and bcopy
data ranges in one allocation. `am_short` stores `[header | payload]` starting
at byte 16, preserving the measured-fast inline layout. `am_bcopy` stores the
packed payload starting at byte 64 because target measurements require aligned
large-fragment writes. A FIFO element carries only one AM type, so the ranges
may overlap without allocating a second per-entry desc array.

Current defaults for both planes:

```text
FIFO_SIZE       = 128
FIFO_ELEM_SIZE  = 131200
BCOPY_SEG_SIZE  = 131072
slot_count      = 96
max_short       = 131184 total AM bytes
max_bcopy       = 131072 bytes
```

The default NC geometry requires 1,612,200,256 bytes, or 1537.514 MiB. With 96
slots, 256 FIFO entries, and 128 KiB elements, the element arrays alone would
consume the full 3 GiB region, so shaving pool/header bytes cannot make doubled
FIFO depth fit while preserving 96 commercial slots and the 128 KiB bcopy cap.
CC uses the same default geometry unless `UCX_OBMM_CC_*` geometry knobs
override it.

Prefer 64-byte-aligned `FIFO_ELEM_SIZE` and `BCOPY_SEG_SIZE` unless new target
measurements prove otherwise. Non-64B-aligned strides have regressed latency on
the current platform.

---

## Wire Format

The active wire format is `UCT_OBMM_WIRE_FORMAT_ZERO_SLOT`.

`uct_obmm_iface_addr_t` carries:

```text
slot_index, pid, plane, wire_format, fifo_size,
fifo_elem_size, bcopy_seg_size
```

`plane` rejects NC/CC cross-wiring. MPI/PML UCX exchanges UCP worker addresses
and UCP records its own address/release version, but it does not prove that a
custom UCT transport was built from the same obmm code. `wire_format` is
therefore the single obmm UCT ABI/code guard. FIFO geometry is carried as the
peer runtime layout so the sender can compute peer FIFO pointers; it is not
compared against the local iface geometry during reachability. `ep_create`
sanity-checks the peer geometry and region size before using it.

---

## Reachability

`obmm_nc`:

- Local iface is created from the local NC export.
- Cross-node peers are reachable when the peer exporter identity matches a
  mapped remote NC import.
- Same-node peers use local NC export loopback only when this MD has no local
  CC export. In the normal dual-plane mode, same-node traffic is left to
  `obmm_cc` or another local TL so `obmm_nc` does not pollute UCP's local-lane
  choice.

`obmm_cc`:

- Local iface is created from the local CC export.
- A peer is reachable only when the peer exporter identity matches the same
  local CC export.
- Remote CC imports are ignored for reachability. Cross-node CC remains
  rejected.
- `obmm_cc` can run as a CC-only, same-node-only TL when only
  `UCX_OBMM_CC_MEMIDS` is configured.

---

## Send Path

`am_short` reserves one peer FIFO slot with explicit LSE CAS on peer `head`.
It writes the FIFO element, copies payload inline after the AM header field,
issues a plane-specific release fence, and publishes the owner bit.

`am_bcopy` reserves one peer FIFO slot, writes the packed payload into the
same shared FIFO data area used by short, issues the same plane-specific
release fence, and publishes a FIFO element with the bcopy flag.

Plane fences:

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

The per-iface receive path polls the local FIFO until the per-call budget is
exhausted or the next expected owner bit is absent. After observing a published
element, the receiver issues the matching plane-specific acquire fence before
reading payload fields.

Inline short payload invokes the AM callback from the FIFO element. Bcopy
payload invokes the AM callback from the same FIFO element data area. Callback
data is valid for callback lifetime only.

OBMM uses the UCX base per-iface progress registration path. If both `obmm_nc`
and `obmm_cc` are active on a worker, each active iface has its own progress
callback and polls only its own FIFO. The earlier shared worker-level OBMM
progress callback was removed after 2026-06-15 target measurements showed no
meaningful performance difference, making the extra worker context, iface list,
and active-count lifecycle unnecessary.

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

`uct_obmm_iface_estimate_perf()` is intentionally retained for both planes.
It reports:

- `UCT_EP_OP_AM_SHORT`: configured bandwidth plus `SHORT_OVERHEAD`.
- `UCT_EP_OP_AM_BCOPY`: configured bandwidth plus `BCOPY_OVERHEAD`.
- unsupported operations: `UCS_ERR_UNSUPPORTED`.

Default tuning knobs:

| Plane | Config Prefix | Default BW | Default Short Overhead | Default Bcopy Overhead |
| --- | --- | --- | --- | --- |
| NC | `UCX_OBMM_NC_*` | `3400MBs` | `1800ns` | `2us` |
| CC | `UCX_OBMM_CC_*` | `12300MBs` | `100ns` | `200ns` |

These defaults are calibrated from the current OSU measurements. NC sustains
about 3.4 GiB/s for large cross-node messages, while two-process same-node CC
sustains about 12.3 GB/s. The fixed short overhead is reported per side, so
the default uses approximately half of the measured minimum message latency.
The high-process-count CC curve is nonlinear and is not represented as a
fabricated per-process or shared-bandwidth constant.

MD-level region classification knobs:

| Config | Meaning |
| --- | --- |
| `UCX_OBMM_NC_MEMIDS` | explicit NC shmdev list |
| `UCX_OBMM_CC_MEMIDS` | explicit same-node CC shmdev list |

UCP protocol-selection logging is intentionally retained. Use
`UCX_PROTO_SELECT_LOG=y` and `UCX_PROTO_SELECT_LOG_RANK=<rank>` to emit
one-shot `ucp_proto_select:` lines for OBMM lanes without mixing with broad
debug logs.

---

## Verification

Local Windows verification is limited to static checks. Do not run `mpirun`,
`ucx_perftest`, or two-node hardware tests locally.

Target checks:

1. Build UCX on Linux.
2. `UCX_TLS=obmm_nc,obmm_cc ucx_info -d` should show `obmm_nc` and `obmm_cc`.
3. `ucx_info -d -t obmm_nc` should show AM short/bcopy, pending, and
   `INTER_NODE`.
4. `ucx_info -d -t obmm_cc` should show AM short/bcopy and pending, with no
   `INTER_NODE` and no `am_zcopy`.
5. `ucx_info -c | grep OBMM` should show `OBMM_NC_*`, `OBMM_CC_*`,
   `OBMM_NC_MEMIDS`, and `OBMM_CC_MEMIDS`, with no legacy `OBMM_MEMIDS` or
   cleanup-time private stats knobs.
6. Validate OSU behavior on the real setup; do not claim target performance
   from local static checks.
