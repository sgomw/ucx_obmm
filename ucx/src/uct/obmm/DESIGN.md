# obmm UCT transport - design notes

This file is the single source of truth for the on-region wire format and the
data-path semantics of the `obmm` UCT transport. Update it before changing
layout, capabilities, or sync rules.

Status: current implementation is an NC AM-only transport with a single shared
FIFO publication path for both `am_short` and `am_bcopy`, plus optional CC
(cache-coherent) acceleration for large am_bcopy messages. The prior
deterministic SPSC `am_short` lane design was removed after 100-process
`osu_multi_lat` runs hung silently at sizes 1024, 4, and 16.

---

## Locked-in environment facts

- Each node pre-exports one NC 256 MiB region and optionally one CC 3 GiB
  region; export/import is done outside UCX.  UCT must not call
  `obmm_export/import/preimport/...`.
- NC data-path mapping is non-cacheable: `open(... O_SYNC)` + `mmap`.
  `obmm_set_ownership` is forbidden and irrelevant for the NC path.
- CC data-path mapping is cacheable: `open(... O_RDWR)` + `mmap(PROT_NONE)`.
  Access requires `obmm_set_ownership()` calls for every read or write.
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

Inside the exported NC region:

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
elem_size       =   2048   (am_short inline capacity = 2040 total bytes)
seg_size        =  19776   (raw UCT max_bcopy = 19776)
slot_stride     = 128 + 128 * (2048 + 19776)
                = 2793600 B
pool_overhead   =   2368 B
total           = 268187968 B = 255.764 MiB / 256 MiB    OK
```

The deeper FIFO is intentional: `am_short` and `am_bcopy` now share the same
ring and the target workload has high process counts. `BCOPY_SEG_SIZE` was
reduced from 32768 to 19776 to keep 96 slots within the 256 MiB region while
doubling FIFO depth to 128, preserving a larger bcopy segment than the
conservative 16 KiB cut, and allowing 1024-byte-class messages to fit in
FIFO-backed `am_short`.

---

## CC Region Layout

The CC export region (up to 3 GiB) is divided into fixed-size buffers.
Each NC FIFO slot N is paired 1:1 with a CC buffer at offset
`N * cc_buf_size` (modulo cc_num_bufs). No separate allocator is needed:
CC buffer lifecycle is naturally gated by the NC FIFO tail -- a buffer
can only be reused when the receiver has released the corresponding NC
FIFO slot.

```
CC export region:
+-------------------------------------------------------+ offset 0
| cc_buf[0]  (cc_buf_size bytes, PMD_SIZE-aligned)     |
+-------------------------------------------------------+
| cc_buf[1]                                             |
+-------------------------------------------------------+
| ...                                                   |
+-------------------------------------------------------+
| cc_buf[cc_num_bufs - 1]                              |
+-------------------------------------------------------+
```

`cc_num_bufs = min(cc_export_size / cc_buf_size, fifo_size)`.

---

## FIFO element layout

`uct_obmm_fifo_element_t` is 16 bytes, packed:

| field      | bytes | notes |
|------------|-------|-------|
| flags      | 1     | OWNER bit + BCOPY bit + CC bit |
| am_id      | 1     | AM id |
| length     | 2     | bcopy/CC payload bytes, or inline short bytes |
| generation | 4     | receiver slot generation token |
| header     | 8     | am_short header; for CC: cc_buffer_offset |

Wire discriminator:

- `flags & UCT_OBMM_FIFO_ELEM_FLAG_CC`: payload lives in CC buffer at
  offset `elem->header` within the sender's CC export region.
- `flags & UCT_OBMM_FIFO_ELEM_FLAG_BCOPY`: payload lives in paired
  `desc[N]` (NC region).
- otherwise: FIFO element carries inline `am_short` data starting at
  `header`.

`elem->length` stays `uint16_t`, so payload sizes are capped at 65535.

---

## Sender side

Both `am_short` and `am_bcopy` (NC or CC) reserve a shared FIFO slot:

### NC am_short / am_bcopy (unchanged)

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

### CC am_bcopy (new)

```
1. Reserve NC FIFO slot (same CAS flow as above, head = N).
2. cc_idx = N & fifo_mask % cc_num_bufs.
3. cc_offset = cc_idx * cc_buf_size.
4. cc_ptr = cc_region->base + cc_offset.
5. obmm_set_ownership(cc_export_fd, cc_ptr, cc_ptr + cc_buf_size,
                       PROT_READ | PROT_WRITE).
6. length = pack_cb(cc_ptr, arg).
7. obmm_set_ownership(cc_export_fd, cc_ptr, cc_ptr + cc_buf_size,
                       PROT_NONE).
8. Fill NC FIFO elem: am_id, length, generation, header=cc_offset.
9. ucs_memory_bus_store_fence().
10. elem[N].flags = OWNER_BIT_FOR_THIS_LAP | FLAG_CC.
```

The OWNER bit alternates each lap of the ring. A CAS is used rather than FAA:
if an FAA claim later discovered "full", the head bump could not be rolled
back and would leave a permanent receiver gap. On aarch64 NC mappings, this CAS
must be the explicit LSE helper.

Pending sends use the same `ucs_arbiter_t` path for all AM operations.

---

## Receiver side

`uct_obmm_iface_progress` drains the shared FIFO:

```
loop up to fifo_poll_count:
    elem = elem[read_index & mask]
    expected_owner = lap_parity(read_index)
    if (elem->flags & OWNER) != expected_owner: break
    ucs_memory_bus_load_fence()
    if elem->generation != iface->generation:
        drop stale slot-reuse data
    elif elem->flags & FLAG_CC:
        cc_offset = elem->header
        cc_ptr = cc_peer_region->base + cc_offset
        obmm_set_ownership(cc_peer_fd, cc_ptr, cc_ptr + length, PROT_READ)
        invoke_am(am_id, cc_ptr, length, 0)
        obmm_set_ownership(cc_peer_fd, cc_ptr, cc_ptr + length, PROT_NONE)
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
from `desc[N]`, inline FIFO bytes, or CC buffer bytes before the receiver
releases the slot.

---

## Capabilities

| flag             | current | notes |
|------------------|---------|-------|
| AM_SHORT         | yes     | max = `fifo_elem_size - offsetof(header)`; default 2040 total bytes |
| AM_BCOPY         | yes     | max = `bcopy_seg_size`; default 19776. CC path available above threshold |
| PENDING          | yes     | queues on shared FIFO backpressure |
| CONNECT_TO_IFACE | yes     | |
| CB_SYNC          | yes     | AM callback data is callback-lifetime only |
| INTER_NODE       | yes     | required for cross-host UCT |

Not advertised: PUT/GET, atomics, AM_ZCOPY, EP_CHECK, AM_DUP,
ERRHANDLE_PEER.

---

## Wire-format compatibility

### Device address (unchanged)

`uct_obmm_device_addr_t` carries `(exporter_dcna, exporter_deid_hi,
exporter_deid_lo)`.

### Iface address

`uct_obmm_iface_addr_t` carries `(slot_index, generation, pid, slot_count,
short_lane_count, fifo_size, fifo_elem_size, bcopy_seg_size,
cc_enabled, cc_buf_size)`.

`short_lane_count` is currently 0. Keeping it on the wire makes this build
incompatible with the removed SPSC-lane layout, which advertised nonzero lanes.
Two ifaces are mutually reachable only when all wire geometry fields match,
including CC geometry. Pool compatibility is also checked against the shared
pool header and slot size during attach/open.

---

## Configuration knobs

All under the `UCX_OBMM_*` prefix.

### MD knobs

| knob           | default | meaning |
|----------------|---------|---------|
| NC_MEMIDS      | ""      | NC shmdev allow-list (comma-separated memids) |
| CC_MEMIDS      | ""      | CC shmdev allow-list (comma-separated memids) |

### Iface knobs

| knob           | default | meaning |
|----------------|---------|---------|
| BW             | 3400MBs | UCP cost-model bandwidth estimate |
| FIFO_SIZE      | 128     | shared ring depth, power of 2 |
| FIFO_ELEM_SIZE | 2048    | bytes per FIFO element; controls `max_short` |
| BCOPY_SEG_SIZE | 19776   | bytes per paired NC desc; controls `max_bcopy` |
| FIFO_MIN_POLL  | 16      | fixed latency-oriented poll floor |
| FIFO_MAX_POLL  | 16      | fixed latency-oriented poll ceiling by default |
| PENDING_QUOTA  | 1       | pending retries per progress call |
| CC_ENABLE      | 0       | non-zero to enable CC acceleration |
| CC_THRESH      | 32768   | min message bytes to route via CC path |
| CC_BUF_SIZE    | 2097152 | CC buffer size (PMD_SIZE-aligned, default 2 MiB) |

Validation at iface init:

- `FIFO_SIZE` > 0 and power of 2
- `FIFO_ELEM_SIZE` > sizeof(`uct_obmm_fifo_element_t`)
- `FIFO_ELEM_SIZE - offsetof(header)` <= `UINT16_MAX`
- `BCOPY_SEG_SIZE` > 0 and <= `UINT16_MAX`
- `slot_count * slot_stride + pool_overhead <= region->length`
- CC: `CC_BUF_SIZE` > 0, CC region size >= `CC_BUF_SIZE`
- CC: `OBMM_CC_MEMIDS` must be non-empty if `CC_ENABLE=1`

The transport does not expose private cleanup-time performance logging knobs;
old `STATS` and `SHORT_PERF_STATS` config entries are not part of the current
code.

---

## Future scope

- `am_zcopy`: requires UCT MD memory-handle plumbing (`mem_reg`, `mkey_pack`,
  `mem_attach`).
- `put_bcopy / get_bcopy`: blocked by the same MD plumbing; current data path
  is AM FIFO only.
- Multi-region per node, NUMA-aware slot placement.
- Variable-size desc allocator if fixed `fifo_size * bcopy_seg_size` footprint
  becomes too costly.
- CC path with per-ep CC import lookup for multi-peer scalability (current
  implementation caches a single CC peer on the iface).

---

## Verification

Per `.github/skills/ucx-build-verify/SKILL.md`:

1. Build on Linux: `./autogen.sh && ./contrib/configure-devel && make -j`.
2. `ucx_info -d -t obmm` should show `am_short` and `am_bcopy`, with
   `max_short` 2040 and `max_bcopy` 19776 by default.
3. `ucx_info -c | grep OBMM` should show the current geometry knobs including
   `CC_ENABLE`, `CC_THRESH`, `CC_BUF_SIZE`.
4. Hardware checks are required: repeat the 100-process
   `osu_multi_lat` cases that previously hung at sizes 1024, 4, and 16;
   measure OSU bandwidth/latency with CC_ENABLE=1 to find the threshold
   where CC outperforms NC.