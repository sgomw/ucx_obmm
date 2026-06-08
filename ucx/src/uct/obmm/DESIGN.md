# OBMM UCT Transport Design

Status: the accepted transport is AM-only and topology-aware. It exposes two
logical UCT TLS under one `obmm` component:

```text
obmm_nc: cross-node AM over non-cacheable NC memory
obmm_cc: same-node AM over cacheable CC memory
```

The two TLS keep UCP performance and reachability decisions separate, while
the transport shares one per-worker OBMM progress engine across active OBMM
ifaces. This avoids the mixed-performance problem of one monolithic TLS and
avoids registering independent NC and CC progress callbacks on the same worker.

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
  `CC_MEMIDS` may be provided alone for same-node-only `obmm_cc`. `obmm_nc`
  uses `NC_MEMIDS`, legacy `MEMIDS`, or the legacy NC-only scan-all mode when
  no explicit list is configured.
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
| `AM_BCOPY` | yes | NC paired-desc FIFO fallback/control path |
| `PENDING` | yes | arbiter-backed retry on FIFO backpressure |
| `CONNECT_TO_IFACE` | yes | endpoint uses peer device and iface addresses |
| `CB_SYNC` | yes | callback data is valid only during callback |
| `INTER_NODE` | yes | NC is the cross-node plane |

`obmm_cc` advertises the same AM/pending/connect capabilities but does not
advertise `INTER_NODE`. Its reachability accepts only peers whose device
address names the same local CC export region.

Not advertised by either plane: `AM_ZCOPY`, PUT/GET/RMA, atomics, `EP_CHECK`,
AM_DUP, and ERRHANDLE_PEER. The ops table must keep unsupported stubs for
unsupported entries.

---

## Pool Geometry

Both planes use the same FIFO/pool layout:

```text
slot_stride = fifo_control + FIFO_SIZE * (FIFO_ELEM_SIZE + BCOPY_SEG_SIZE)
required    = pool_header + 96 * slot_stride
```

Current defaults for both planes:

```text
FIFO_SIZE       = 64
FIFO_ELEM_SIZE  = 520128
BCOPY_SEG_SIZE  = 4096
slot_count      = 96
max_short       = 520112 total AM bytes
max_bcopy       = 4096 bytes
```

The default NC geometry requires 3,220,846,912 bytes, or 3071.639 MiB. CC uses
the same default geometry unless `UCX_OBMM_CC_*` geometry knobs override it.

Prefer 64-byte-aligned `FIFO_ELEM_SIZE` and `BCOPY_SEG_SIZE` unless new target
measurements prove otherwise. Non-64B-aligned strides have regressed latency on
the current platform.

---

## Wire Format

The active wire format is `UCT_OBMM_WIRE_FORMAT_INLINE32`.

`uct_obmm_iface_addr_t` carries:

```text
slot_index, generation, pid, plane, slot_count, short_lane_count,
wire_format, fifo_size, fifo_elem_size, bcopy_seg_size
```

`plane` rejects NC/CC cross-wiring. `short_lane_count` is kept as 0 to reject
peers built with the removed dedicated SPSC short-lane layout. Reachability and
`ep_create` reject peers whose plane, slot count, wire format, FIFO size, FIFO
element size, bcopy segment size, or generation are incompatible.

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
paired descriptor area, issues the same plane-specific release fence, and
publishes a FIFO element with the bcopy flag. Its size is intentionally small.

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
payload invokes the AM callback from the paired descriptor. Callback data is
valid for callback lifetime only.

Unlike the UCX base progress path, OBMM does not register one progress callback
per iface. `obmm_nc` and `obmm_cc` ifaces attached to the same UCT worker are
linked into a shared worker-level OBMM engine. The first active OBMM iface
registers one callback in the worker progress queue; subsequent active OBMM
ifaces share that callback.

On slot reuse or process cleanup, pool metadata reset clears stale geometry and
allocation state without zeroing the full region.

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
| NC | `UCX_OBMM_NC_*` | `3400MBs` | `100ns` | `2us` |
| CC | `UCX_OBMM_CC_*` | `50000MBs` | `50ns` | `1us` |

MD-level region classification knobs:

| Config | Meaning |
| --- | --- |
| `UCX_OBMM_MEMIDS` | legacy NC-only shmdev list |
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
5. `ucx_info -c | grep OBMM` should show `OBMM_NC_*`, `OBMM_CC_*`, and the
   MD-level memid knobs, with no cleanup-time private stats knobs.
6. Validate OSU behavior on the real setup; do not claim target performance
   from local static checks.
