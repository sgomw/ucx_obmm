# obmm UCT transport - design notes

This file is the single source of truth for the on-region wire format and the
data-path semantics of the `obmm` UCT transport. Update it before changing
layout, capabilities, or sync rules.

Status: current implementation keeps the NC short-first path with a single
shared FIFO publication path for both `am_short` and `am_bcopy`, and adds a
conditional sender-staged CC `am_zcopy` path. NC inline FIFO carries as much
eager payload as the current NC region allows, while bcopy is kept as a small
UCP wireup/control/fallback path. CC AM_ZCOPY is advertised only when explicit
NC/CC region classification succeeds and a cacheable CC export is available.

---

## Locked-in environment facts

- Each node pre-exports one 3 GiB NC region; export/import is done outside UCX.
  The current 96-slot short-first geometry requires 3,220,846,912 B
  (3071.639 MiB). UCT must not call `obmm_export/import/preimport/...`.
- The approved CC staged AM_ZCOPY phase also uses one externally exported
  cacheable CC region and one imported peer CC region per node. The user must
  classify which discovered shmdevs are NC and which are CC; the transport
  must not infer the type from memid alone.
- NC short/control mapping is non-cacheable: `open(... O_SYNC)` + `mmap`.
  `obmm_set_ownership` is forbidden and irrelevant for the NC path.
- CC payload mapping is cacheable: open without `O_SYNC`, map with
  `MAP_SHARED`, and use page-aligned `obmm_set_ownership` transitions only for
  staged AM_ZCOPY payload chunks. The transport resolves
  `obmm_set_ownership` dynamically from `libobmm.so` / `libobmm.so.0` at
  runtime; UCX configure is not hard-wired to libobmm.
- Cross-host atomic RMW on NC is supported only through explicit arm64 LSE
  instructions. Compiler-default LL/SC atomics are unusable on NC mappings.
- Memory ordering uses bus-domain fences
  (`ucs_memory_bus_store_fence` / `ucs_memory_bus_load_fence`), not the
  CPU-domain fences that mm uses.
- Reachability is keyed on `(exporter_dcna, exporter_deid, memid)`, never
  memid alone.
- Pool init is two-phase: UNINIT -> INITING -> READY via CAS, with per-slot
  `(generation, owner_pid, owner_starttime)` for crash/PID-reuse safety.

---

## Region layout

Inside the exported region:

```
+-------------------------------------------------------+ offset 0
| uct_obmm_pool_hdr_t (magic, state, geometry)          |
+-------------------------------------------------------+
| alloc_bitmap[bitmap_words]                            |
+-------------------------------------------------------+
| slot_meta[slot_count]  (gen, owner_pid, starttime, ... )|
+-------------------------------------------------------+ hdr->slot_array_offset
| slot[0]:                                              |
|   uct_obmm_fifo_ctl_t  (head + tail, padded)          |
|   fifo_elem[fifo_size]  (elem_size each)              |
|   bcopy_desc[fifo_size] (seg_size each)               |
+-------------------------------------------------------+
| slot[1]: ...                                          |
...
```

Every FIFO element `elem[N]` has a paired `desc[N]` of `seg_size` bytes in
the same slot. `desc[N]` lifetime is identical to `elem[N]`: both are released
when the receiver bumps tail past index N, and both are overwritten by the
sender that claims index N + fifo_size.

### Slot stride

Current defaults:

```
slot_count      =      96
fifo_size       =      64
elem_size       =  520128   (am_short inline capacity = 520112 total bytes)
seg_size        =    4096   (raw UCT max_bcopy = 4096)
slot_stride     = 128 + 64 * (520128 + 4096)
                = 33550464 B
pool_overhead   =    2368 B
total           = 3220846912 B = 3071.639 MiB / 2.999 GiB
```

This geometry intentionally uses nearly all of the current 3 GiB NC region to
maximize the short-covered range for crossover testing. If the target export is
smaller than exactly 3 GiB, reduce `UCX_OBMM_FIFO_ELEM_SIZE`; 516096 still
covers the 256 KiB OSU size while preserving the same 4 KiB bcopy default.

---

## FIFO element layout

`uct_obmm_fifo_element_t` is 24 bytes, packed, with explicitly aligned fields:

| field      | bytes | notes |
|------------|-------|-------|
| flags      | 1     | OWNER bit + optional BCOPY bit |
| am_id      | 1     | AM id |
| reserved0  | 2     | aligns `length` |
| length     | 4     | bcopy payload bytes, or short `[header|payload]` bytes |
| generation | 4     | receiver slot generation token |
| reserved1  | 4     | aligns `header` |
| header     | 8     | am_short header; unused for bcopy |

Wire discriminator:

- `flags & UCT_OBMM_FIFO_ELEM_FLAG_BCOPY`: payload is in paired `desc[N]`.
- otherwise: FIFO element carries inline `am_short` data starting at `header`.

`elem->length` is 32-bit in wire format `UCT_OBMM_WIRE_FORMAT_INLINE32`.
The raw FIFO short capacity is `FIFO_ELEM_SIZE - offsetof(header)`. In NC-only
mode this is the advertised `max_short`; when CC AM_ZCOPY is enabled, the
advertised `max_short` is capped at `CC_MIN_ZCOPY - 1` so UCP does not keep
selecting NC short for messages intended to cross over to CC zcopy.

---

## Sender side

Both `am_short` and `am_bcopy` reserve a shared FIFO slot:

```
1. load head from peer_ctl->head
2. if (head - cached_tail) >= fifo_size:
       bus_load_fence; refresh cached_tail; recheck;
       if still full: return UCS_ERR_NO_RESOURCE
3. CAS head -> head+1
4. N = head & mask
5. payload write:
     short: write elem[N].header and inline payload after it
     bcopy: pack_cb(desc[N], arg) -> length
6. fill elem[N] metadata (am_id, length, generation, header/0)
7. ucs_memory_bus_store_fence()
8. elem[N].flags = OWNER_BIT_FOR_THIS_LAP | (BCOPY if bcopy)
```

The OWNER bit alternates each lap of the ring. A CAS is used rather than FAA:
if an FAA claim later discovered "full", the head bump could not be rolled
back and would leave a permanent receiver gap. On aarch64 NC mappings, this CAS
must be the explicit LSE helper.

Pending sends use the same `ucs_arbiter_t` path for both AM operations.

---

## Receiver side

`uct_obmm_iface_progress` drains the shared FIFO only:

```
loop up to fifo_poll_count:
    elem = elem[read_index & mask]
    expected_owner = lap_parity(read_index)
    if (elem->flags & OWNER) != expected_owner: break
    ucs_memory_bus_load_fence()
    if elem->generation != iface->generation:
        drop stale slot-reuse data
    elif elem->flags & BCOPY:
        invoke_am(am_id, desc[read_index & mask], length, 0)
    else:
        invoke_am(am_id, &elem->header, length, 0)
    read_index++
if any progress:
    uct_obmm_bus_full_fence()
    recv_ctl->tail = read_index
dispatch pending retries
```

The full bus fence before tail publication is required because AM handlers load
from `desc[N]` or inline FIFO bytes before the receiver releases the slot.

---

## Capabilities

| flag             | current | notes |
|------------------|---------|-------|
| AM_SHORT         | yes     | NC-only max = raw FIFO short capacity; CC-enabled max = min(raw capacity, `CC_MIN_ZCOPY - 1`) |
| AM_BCOPY         | yes     | max = `bcopy_seg_size`; default 4096 |
| AM_ZCOPY         | conditional | sender-staged cacheable CC payload path when CC setup succeeds |
| PENDING          | yes     | queues on shared FIFO backpressure |
| CONNECT_TO_IFACE | yes     | |
| CB_SYNC          | yes     | AM callback data is callback-lifetime only |
| INTER_NODE       | yes     | required for cross-host UCT |

Not advertised in the NC-only baseline: PUT/GET, atomics, AM_ZCOPY, EP_CHECK,
AM_DUP,
ERRHANDLE_PEER.

When CC is enabled, AM_ZCOPY caps are:

- `min_zcopy = 0`; UCP proto-v2 rejects AM zcopy lanes with nonzero
  `cap.am.min_zcopy`
- `max_zcopy = UCX_OBMM_CC_CHUNK_SIZE` (default 1 MiB)
- `max_iov = UCX_OBMM_CC_MAX_IOV` (default 8)

`UCX_OBMM_CC_MIN_ZCOPY` is still the NC/CC crossover knob: it caps advertised
`max_short` when CC is enabled and should match the user's `UCX_ZCOPY_THRESH`.

`AM_BCOPY` is intentionally small. UCP's hard wireup floor is 64 B, and obmm
defaults to 4 KiB to keep wireup/control headroom without making bcopy the
medium-message performance path.

---

## Wire-format compatibility

`uct_obmm_iface_addr_t` carries `(slot_index, generation, pid, slot_count,
short_lane_count, wire_format, fifo_size, fifo_elem_size, bcopy_seg_size,
cc_chunk_count, cc_chunk_size, cc_min_zcopy)`.

`short_lane_count` is currently 0. Keeping it on the wire makes this build
incompatible with the removed SPSC-lane layout, which advertised nonzero lanes.
`wire_format` is `UCT_OBMM_WIRE_FORMAT_INLINE32` for NC-only operation and
`UCT_OBMM_WIRE_FORMAT_CCZCOPY` when CC staged AM_ZCOPY is enabled, rejecting
peers that lack matching CC control/geometry support. Two ifaces are mutually
reachable only when all wire geometry fields match. Pool compatibility is also
checked against the shared pool header magic and slot size during attach/open.
Final cleanup resets header/bitmap/meta only; slot payload bytes are zeroed
when a slot is allocated. This avoids clearing the full 3 GiB NC region during
process teardown.

---

## Configuration knobs

All under the `UCX_OBMM_*` prefix.

| knob           | default | meaning |
|----------------|---------|---------|
| BW             | 3400MBs | UCP cost-model bandwidth estimate |
| FIFO_SIZE      | 64      | shared ring depth, power of 2 |
| FIFO_ELEM_SIZE | 520128  | bytes per FIFO element; controls raw short capacity |
| BCOPY_SEG_SIZE | 4096    | bytes per paired desc; controls fallback `max_bcopy` |
| FIFO_MIN_POLL  | 16      | fixed latency-oriented poll floor |
| FIFO_MAX_POLL  | 16      | fixed latency-oriented poll ceiling by default |
| PENDING_QUOTA  | 1       | pending retries per progress call |
| MEMIDS         | ""      | optional comma-separated shmdev memids |
| NC_MEMIDS      | ""      | explicit NC shmdev memids; do not combine with `MEMIDS` |
| CC_MEMIDS      | ""      | explicit cacheable CC shmdev memids |
| CC_MIN_ZCOPY   | 256K    | NC/CC crossover size; caps advertised `max_short` when CC is enabled |
| CC_CHUNK_SIZE  | 1M      | bytes per sender-owned CC staging chunk; `max_zcopy` |
| CC_CHUNK_COUNT | 4       | sender-owned CC chunks per local iface slot |
| CC_MAX_IOV     | 8       | advertised AM_ZCOPY max_iov |

Validation at iface init:

- `FIFO_SIZE` > 0 and power of 2
- `FIFO_ELEM_SIZE` > sizeof(`uct_obmm_fifo_element_t`)
- `BCOPY_SEG_SIZE` >= 64
- `slot_count * slot_stride + pool_overhead <= region->length`
- CC chunk size and per-slot stride are page-aligned and fit in the CC region
- CC export identity must match the NC export identity so device address can
  key both NC control and CC payload regions by exporter identity plus kind

The transport does not expose private cleanup-time performance logging knobs;
old `STATS` and `SHORT_PERF_STATS` config entries are not part of the current
code.

---

## IOV status

UCT does not provide `am_bcopy_iov`. The relevant IOV entry points are
`ep_am_short_iov` and `ep_am_zcopy`.

`ep_am_short_iov` is not implemented in this phase because MPI tag contiguous
OSU traffic uses ordinary `uct_ep_am_short`, and adding short_iov would add a
new variable before the NC/CC crossover is measured. Revisit it for UCP AM
workloads with separate user headers or non-tag traffic.

---

## CC staged AM_ZCOPY direction

- Keep NC FIFO as the control plane for small AM, CC slot credits,
  `CC_DATA_READY`, ACKs, and pending retry progress.
- Keep `am_bcopy` small; it is a UCP wireup/control/fallback path, not a
  performance path.
- Use cacheable CC only for large payload chunks. All ownership ranges must be
  page-aligned and must not overlap between local process slots.
- Start with sender-staging: sender writes to its own local/exported CC chunk,
  drops ownership to no-access to flush, then notifies the receiver over NC.
  Receiver raises read ownership on the imported sender CC chunk, invokes the
  AM callback synchronously, drops ownership, and returns the chunk credit over
  NC. This keeps CC chunks owned by the sending process and avoids
  multi-sender collisions in a receiver-owned pool.
- Separate source-buffer lifetime from CC chunk lifetime. `ep_am_zcopy`
  returns `UCS_OK` after the sender has copied source iovs into CC, released
  write ownership, and published `CC_DATA_READY`; the UCP source buffer may be
  reused then. The CC chunk is reusable only after receiver ACK.
- Initial test policy: use the measured crossover as `CC_MIN_ZCOPY`
  (start with 256 KiB, compare 192 KiB), keep advertised `min_zcopy=0`, and
  use a bounded chunk size such as 1 MiB for `max_zcopy` rather than exposing
  the full CC region.
- `ep_am_zcopy` must track receiver ACK asynchronously and integrate with
  pending retry plus `ep_flush` / `iface_flush`; current AM-only flush behavior
  is not sufficient after zcopy is added.

---

## Future scope

- `ep_am_short_iov` if UCP AM IOV workloads become a target.
- Multi-region per node, NUMA-aware slot placement.

---

## Verification

Per `.github/skills/ucx-build-verify/SKILL.md`:

1. Build on Linux: `./autogen.sh && ./contrib/configure-devel && make -j`.
2. `ucx_info -d -t obmm` should show `am_short` and `am_bcopy`. In NC-only
   mode, default `max_short` is 520112 and `max_bcopy` is 4096. With
   `CC_MEMIDS`, `max_short` should be capped below `CC_MIN_ZCOPY` and
   `am_zcopy` should show `min_zcopy=0`, the configured `max_zcopy`, and
   `max_iov`.
3. `ucx_info -c | grep OBMM` should show the current geometry knobs and should
   not show removed private stats knobs.
4. Hardware checks are required for this short-first + CC geometry: run the OSU
   sweep with `UCX_OBMM_NC_MEMIDS`, `UCX_OBMM_CC_MEMIDS`, and multiple
   `UCX_RNDV_THRESH` / `UCX_ZCOPY_THRESH` values to validate the measured
   NC/CC crossover.
