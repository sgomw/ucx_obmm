# obmm UCT transport - design notes

This file is the single source of truth for the on-region wire format and the
data-path semantics of the `obmm` UCT transport. Update it before changing
layout, capabilities, or sync rules.

Status: current implementation is an NC AM-only transport with a single shared
FIFO publication path for both `am_short` and `am_bcopy`. The prior
deterministic SPSC `am_short` lane design was removed after 100-process
`osu_multi_lat` runs hung silently at sizes 1024, 4, and 16. The NC bcopy
segment default is now 192 KiB as an interim medium-message path; final large
messages are planned to use a separate CC-backed `am_zcopy`/rendezvous path.

---

## Locked-in environment facts

- Each node pre-exports one NC region; export/import is done outside UCX. The
  default 96-slot / 192 KiB bcopy geometry requires at least 2,441,099,584 B.
  UCT must not call `obmm_export/import/preimport/...`.
- Data-path mapping is non-cacheable: `open(... O_SYNC)` + `mmap`.
  `obmm_set_ownership` is forbidden and irrelevant for the current NC path.
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
| slot_meta[slot_count]  (gen, owner_pid, starttime, ...)|
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
slot_count      =     96
fifo_size       =    128
elem_size       =   2048   (am_short inline capacity = 2032 total bytes)
seg_size        = 196608   (raw UCT max_bcopy = 196608)
slot_stride     = 128 + 128 * (2048 + 196608)
                = 25428096 B
pool_overhead   =   2368 B
total           = 2441099584 B = 2328.014 MiB / 2.273 GiB
```

The deeper FIFO is intentional: `am_short` and `am_bcopy` now share the same
ring and the target workload has high process counts. `BCOPY_SEG_SIZE` is kept
large enough to reduce UCP fragmentation for medium messages, but not large
enough to turn NC into the final large-message data path. The 192 KiB default
stays comfortably under a 16 GiB node budget and leaves the later CC path to
handle multi-MiB transfers.

---

## FIFO element layout

`uct_obmm_fifo_element_t` is 24 bytes, with explicit padding so the 64-bit AM
header remains naturally aligned in NC memory:

| field      | bytes | notes |
|------------|-------|-------|
| flags      | 1     | OWNER bit + optional BCOPY bit |
| am_id      | 1     | AM id |
| reserved0  | 2     | wire padding |
| length     | 4     | bcopy payload bytes, or short `[header|payload]` bytes |
| generation | 4     | receiver slot generation token |
| reserved1  | 4     | wire padding |
| header     | 8     | am_short header; unused for bcopy |

Wire discriminator:

- `flags & UCT_OBMM_FIFO_ELEM_FLAG_BCOPY`: payload is in paired `desc[N]`.
- otherwise: FIFO element carries inline `am_short` data starting at `header`.

`elem->length` is `uint32_t`. OBMM's own length field is not the active cap;
the real advertised cap is constrained by UCP's AM segment-size propagation,
which uses 64-byte units in a 16-bit packed field. Therefore `BCOPY_SEG_SIZE`
must be 64-byte aligned and no larger than `65535 * 64 = 4194240` bytes.
`FIFO_ELEM_SIZE - offsetof(header)` is the advertised `max_short`.

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
| AM_SHORT         | yes     | max = `fifo_elem_size - offsetof(header)`; default 2032 total bytes |
| AM_BCOPY         | yes     | max = `bcopy_seg_size`; default 196608 |
| PENDING          | yes     | queues on shared FIFO backpressure |
| CONNECT_TO_IFACE | yes     | |
| CB_SYNC          | yes     | AM callback data is callback-lifetime only |
| INTER_NODE       | yes     | required for cross-host UCT |

Not advertised: PUT/GET, atomics, AM_ZCOPY, EP_CHECK, AM_DUP,
ERRHANDLE_PEER.

---

## Wire-format compatibility

`uct_obmm_iface_addr_t` carries `(slot_index, generation, pid, slot_count,
short_lane_count, fifo_size, fifo_elem_size, bcopy_seg_size)`.

`short_lane_count` is currently 0. Keeping it on the wire makes this build
incompatible with the removed SPSC-lane layout, which advertised nonzero lanes.
Two ifaces are mutually reachable only when all wire geometry fields match.
Pool compatibility is also checked against the shared pool header and slot
size during attach/open.

---

## Configuration knobs

All under the `UCX_OBMM_*` prefix.

| knob           | default | meaning |
|----------------|---------|---------|
| BW             | 3400MBs | UCP cost-model bandwidth estimate |
| FIFO_SIZE      | 128     | shared ring depth, power of 2 |
| FIFO_ELEM_SIZE | 2048    | bytes per FIFO element; controls `max_short` |
| BCOPY_SEG_SIZE | 196608  | bytes per paired desc; controls `max_bcopy` |
| FIFO_MIN_POLL  | 16      | fixed latency-oriented poll floor |
| FIFO_MAX_POLL  | 16      | fixed latency-oriented poll ceiling by default |
| PENDING_QUOTA  | 1       | pending retries per progress call |
| MEMIDS         | ""      | optional comma-separated shmdev memids |

Validation at iface init:

- `FIFO_SIZE` > 0 and power of 2
- `FIFO_ELEM_SIZE` > sizeof(`uct_obmm_fifo_element_t`)
- `BCOPY_SEG_SIZE` > 0
- `BCOPY_SEG_SIZE` is 64-byte aligned
- `BCOPY_SEG_SIZE` <= 4,194,240, the UCP packed AM segment-size ceiling
- `slot_count * slot_stride + pool_overhead <= region->length`

The transport does not expose private cleanup-time performance logging knobs;
old `STATS` and `SHORT_PERF_STATS` config entries are not part of the current
code.

---

## Future scope

- CC-backed `am_zcopy` or AM rendezvous: use NC FIFO for control and a bounded
  CC credit/window pool for large-message data. Keep the CC pool shared across
  peers rather than allocating `fifo_size * max_zcopy` bytes per slot.
- `am_zcopy`: requires UCT MD memory-handle plumbing (`mem_reg`, `mkey_pack`,
  `mem_attach`) or an explicit AM rendezvous design that preserves UCP/UCT
  callback lifetime and completion semantics.
- `put_bcopy / get_bcopy`: blocked by the same MD plumbing; current data path
  is AM FIFO only.
- Multi-region per node, NUMA-aware slot placement.
- Variable-size desc allocator if fixed `fifo_size * bcopy_seg_size` footprint
  becomes too costly.

---

## Verification

Per `.github/skills/ucx-build-verify/SKILL.md`:

1. Build on Linux: `./autogen.sh && ./contrib/configure-devel && make -j`.
2. `ucx_info -d -t obmm` should show `am_short` and `am_bcopy`, with
   `max_short` 2032 and `max_bcopy` 196608 by default.
3. `ucx_info -c | grep OBMM` should show the current geometry knobs and should
   not show removed private stats knobs.
4. Hardware checks are required for this rollback: repeat the 100-process
   `osu_multi_lat` cases that previously hung at sizes 1024, 4, and 16.
