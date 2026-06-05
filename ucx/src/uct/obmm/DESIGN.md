# OBMM UCT Transport Design

Status: the accepted transport is NC-only, AM-only, and short-first. The data
path uses one shared non-cacheable FIFO publication path for both `am_short`
and `am_bcopy`. Large eager payload should be carried by NC inline short as far
as the NC region geometry allows; `am_bcopy` is kept small for UCP
wireup/control/fallback and for protocol tuning experiments.

Cross-node cacheable CC as a transport data path was explored and rejected on
2026-06-05. Do not implement or tune staged CC zcopy, sender-owned CC,
receiver-owned CC, or CC batch/epoch paths in this UCT transport unless the
user explicitly opens a new design.

---

## Rejected CC Direction

The project tried larger NC bcopy, sender-owned CC, receiver-owned staged
zcopy, tiny-fragment fallback, and batch/epoch probing. The route was rejected
for these reasons:

- CC ownership transitions dominate the path. The sender writes into cache and
  immediately releases ownership, forcing writeback; the receiver reads into
  cache and immediately releases ownership again.
- Effective ownership granularity on the target behaves PMD/2 MiB-like, so
  small protocol fragments and sub-granule chunks still pay large-range
  transition costs.
- UCP AM zcopy semantics are single-message oriented and do not naturally
  expose the batching needed to amortize ownership.
- Batch/epoch probing showed throughput speedups only at larger batches, but
  that would require large windows and additional queueing latency. That shape
  is a throughput protocol, not an OSU `multi_lat` latency path.
- Under high concurrency, the software lifecycle around ownership, copy,
  callback, ACK, and credit management regressed compared with the NC-only
  baseline.

The accepted conclusion is NC short-first. Keep UCP protocol-selection logging
and `iface_estimate_perf()` for NC-only tuning, but do not advertise AM zcopy
or use `obmm_set_ownership()` in the UCT transport.

---

## Environment Facts

- Export/import is done outside UCX. UCT must not call `obmm_export`,
  `obmm_unexport`, `obmm_import`, `obmm_unimport`, `obmm_preimport`, or
  `obmm_unpreimport`.
- Each node currently provides one 3 GiB NC export region and imports the peer
  node's NC export.
- NC mappings are opened as `open(..., O_RDWR | O_SYNC)` and mapped with
  `MAP_SHARED | PROT_READ | PROT_WRITE`.
- Do not call `obmm_set_ownership()` from the UCT transport. It is irrelevant
  to the NC path and the cross-node CC route is rejected.
- Peer matching is by exporter identity, not memid. Use exporter DCNA/DEID
  from sysfs `export_info`/`import_info`.
- On arm64 NC mappings, shared control-word atomic RMW must use explicit LSE
  instructions. Do not rely on compiler-lowered LL/SC atomics or generic
  `ucs_atomic_*` for shared NC control words.

---

## Capabilities

Advertised UCT capabilities:

| Capability | Status | Notes |
| --- | --- | --- |
| `AM_SHORT` | yes | NC FIFO inline payload; default `max_short` is 520112 total bytes |
| `AM_BCOPY` | yes | NC paired-desc FIFO path; default `max_bcopy` is 4096 |
| `PENDING` | yes | arbiter-backed retry on FIFO backpressure |
| `CONNECT_TO_IFACE` | yes | endpoint uses peer device and iface addresses |
| `CB_SYNC` | yes | callback data is valid only during callback |
| `INTER_NODE` | yes | required for cross-host use |

Not advertised: `AM_ZCOPY`, PUT/GET/RMA, atomics, `EP_CHECK`, AM_DUP, and
ERRHANDLE_PEER. The ops table must keep unsupported stubs for unsupported
entries.

---

## Pool Geometry

The current default 96-slot NC geometry is:

| Config | Default | Meaning |
| --- | --- | --- |
| `FIFO_SIZE` | 64 | shared FIFO depth, power of two |
| `FIFO_ELEM_SIZE` | 520128 | bytes per FIFO inline element |
| `BCOPY_SEG_SIZE` | 4096 | paired descriptor bytes and advertised `max_bcopy` |

Required NC bytes:

```text
slot_stride = fifo_control + FIFO_SIZE * (FIFO_ELEM_SIZE + BCOPY_SEG_SIZE)
required    = pool_header + 96 * slot_stride
            = 3,220,846,912 bytes
            = 3071.639 MiB
```

Prefer 64-byte-aligned `FIFO_ELEM_SIZE` and `BCOPY_SEG_SIZE` unless new target
measurements prove otherwise. Non-64B-aligned strides have regressed latency on
the current platform.

`FIFO_ELEM_SIZE` controls the raw inline short capacity:

```text
max_short = FIFO_ELEM_SIZE - offsetof(uct_obmm_fifo_element_t, header)
```

With the default geometry, `max_short` is 520112 total AM bytes.

---

## Wire Format

The NC-only wire format is `UCT_OBMM_WIRE_FORMAT_INLINE32`.

`uct_obmm_iface_addr_t` carries:

```text
slot_index, generation, pid, slot_count, short_lane_count,
wire_format, fifo_size, fifo_elem_size, bcopy_seg_size
```

`short_lane_count` is kept as 0 to reject peers built with the removed
dedicated SPSC short-lane layout. Reachability and `ep_create` reject peers
whose slot count, wire format, FIFO size, FIFO element size, bcopy segment
size, or generation are incompatible.

---

## Send Path

`am_short` reserves one peer FIFO slot with explicit LSE CAS on peer `head`.
It writes the FIFO element, copies payload inline after the AM header field,
issues a bus store fence, and publishes the owner bit.

`am_bcopy` also reserves one FIFO slot, but writes the packed payload into the
paired descriptor area. It publishes a FIFO element with the bcopy flag after a
bus store fence. Its size is intentionally small.

If the peer FIFO is full, send returns `UCS_ERR_NO_RESOURCE`. `pending_add`
queues requests on an iface arbiter and preserves FIFO order by not returning
`UCS_ERR_BUSY` when older queued requests exist for that endpoint.

`ep_flush` and `iface_flush` complete immediately because the NC AM path has no
asynchronous operation once a FIFO element has been published.

---

## Receive Path

Progress polls the local FIFO until the per-call budget is exhausted or the
next expected owner bit is absent. After observing a published element, the
receiver issues a bus load fence before reading payload fields.

Inline short payload invokes the AM callback from the FIFO element. Bcopy
payload invokes the AM callback from the paired descriptor. Callback data is
valid for callback lifetime only.

On slot reuse or process cleanup, pool metadata reset clears stale geometry and
allocation state without zeroing the full 3 GiB region.

---

## UCP Tuning Hooks

`uct_obmm_iface_estimate_perf()` is intentionally retained. It reports:

- `UCT_EP_OP_AM_SHORT`: configured bandwidth plus `SHORT_OVERHEAD`.
- `UCT_EP_OP_AM_BCOPY`: configured bandwidth plus `BCOPY_OVERHEAD`.
- unsupported operations: `UCS_ERR_UNSUPPORTED`.

Default tuning knobs:

| Config | Default | Meaning |
| --- | --- | --- |
| `BW` | 3400MBs | UCP cost-model bandwidth estimate |
| `SHORT_OVERHEAD` | 100ns | UCP cost-model short overhead |
| `BCOPY_OVERHEAD` | 2us | UCP cost-model bcopy overhead |
| `PENDING_QUOTA` | 1 | pending retries per progress call |
| `MEMIDS` | "" | legacy optional NC shmdev memids |
| `NC_MEMIDS` | "" | explicit NC shmdev memids; do not combine with `MEMIDS` |

UCP protocol-selection logging is also intentionally retained. Use
`UCX_PROTO_SELECT_LOG=y` and `UCX_PROTO_SELECT_LOG_RANK=<rank>` to emit
one-shot `ucp_proto_select:` lines for obmm lanes without mixing with broad
debug logs.

---

## Verification

Local Windows verification is limited to static checks. Do not run `mpirun`,
`ucx_perftest`, or two-node hardware tests locally.

Target checks:

1. Build UCX on Linux.
2. `ucx_info -d -t obmm` should show `am_short`, `am_bcopy`, pending, and no
   `am_zcopy`.
3. `ucx_info -c | grep OBMM` should show only NC geometry/perf/memid knobs and
   no `UCX_OBMM_CC_*` knobs or private cleanup-time stats knobs.
4. Use `UCX_PROTO_SELECT_LOG=y` when diagnosing UCP choices.
5. Validate OSU behavior on the real two-node setup; do not claim target
   performance from local static checks.
