# obmm UCT transport — design notes

This file is the **single source of truth** for the on-region wire format
and the data-path semantics of the `obmm` UCT transport. Update it
**before** changing layout, capabilities, or sync rules. AGENTS.md
mandates retrieve-before-recall; this doc is the first thing to grep.

Status legend:
- **v1** = shipped, MPI cross-node smoke tests pass.
- **v2** = current target: complete `am_bcopy` (max_bcopy decoupled from
  FIFO element size, no per-message UCP fragmentation below seg_size).

---

## Locked-in environment facts

(Mirrors `.github/skills/obmm-api-and-env/SKILL.md`. Do NOT contradict
without re-checking that file.)

- Each node pre-exports **one 128 MiB region**; export/import is done
  outside UCX. UCT must NOT call `obmm_export/import/preimport/...`.
- Data-path mapping is **non-cacheable** (`open(... O_SYNC)` + mmap).
  `obmm_set_ownership` is forbidden and irrelevant.
- Cross-host 64-bit FAA/CAS on the target NC mapping is **not** reliable enough
  for transport ownership or queue reservation. Standalone probe results showed
  non-monotonic FAA return values and CAS/readback mismatches, so the protocol
  must avoid shared cross-node atomic RMW on hot-path state.
- Memory ordering uses **bus-domain fences**
  (`ucs_memory_bus_store_fence` / `ucs_memory_bus_load_fence`), NOT the
  CPU-domain `ucs_memory_cpu_*_fence` that mm uses. mm peers share an
  inner-shareable cache domain; obmm peers do not.
- Reachability is keyed on `(exporter_dcna, exporter_deid, memid)`,
  never memid alone.
- Two-phase pool init (UNINIT → INITING → READY via CAS) plus per-slot
  `(generation, owner_pid, owner_starttime)` for crash/PID-reuse safety.
  During teardown, INITING is reused only as a transient cleanup lock; after
  reset the shared region returns to literal all-zero memory.
- ARM64 is the production ISA. NC + bus fences are correct on aarch64.

---

## Region layout (v2)

Inside the 128 MiB exported region:

```
+-------------------------------------------------------+ offset 0
| uct_obmm_pool_hdr_t (magic, state, geometry)          |
+-------------------------------------------------------+
| alloc_bitmap[bitmap_words]                            |
+-------------------------------------------------------+
| slot_meta[slot_count]  (gen, owner_pid, starttime, …) |
+-------------------------------------------------------+ hdr->slot_array_offset
| slot[0]:                                              |
|   bank[local]: lane[0..slot_count-1]                  |
|   bank[remote]: lane[0..slot_count-1]                 |
+-------------------------------------------------------+
| slot[1]: …                                            |
…
```

Each slot is **sender-owned** by exactly one iface. Inside that slot:

- there are two banks: `local` and `remote`
- each bank has one lane per destination slot index
- each lane is SPSC:
  - slot owner is the only writer of `head` and `sender_generation`
  - the matching receiver is the only writer of `tail` and `tail_generation`

Banking is derived from exporter identity relative to the sender's local export
region, not from slot index. This avoids local-slot-index and remote-slot-index
collisions in the fixed two-node topology.

Every lane element `elem[N]` has a paired `desc[N]` in that same lane. Their
lifetime is identical: once the receiver advances `tail` past index `N`, the
sender may reuse both on the next lap.

### Slot stride

```
lane_stride = align_up(
    sizeof(uct_obmm_mailbox_ctl_t) +
    fifo_size * elem_size +
    fifo_size * seg_size,
    cacheline)

slot_stride = align_up(
    2 * slot_count * lane_stride,
    cacheline)
```

The pool's `slot_count` (compile-time `UCT_OBMM_POOL_SLOT_COUNT = 32`)
**times** `slot_stride` MUST fit in `region->length` (128 MiB minus pool
 header overhead). Defaults are picked to favor short-path coverage over
 maximum local process count; lowering `seg_size` and/or `fifo_size`
 reduces per-slot footprint, but the supported local attach count remains
 the compile-time `slot_count`.

Default budget check:
```
fifo_size       =      1
elem_size       =  16448   (raw UCT max_short = 16432 total bytes)
seg_size        =  32768   (raw UCT max_bcopy = 32768)
lane_stride     =  49344
ctl + slot data = 64 * 49344 = ~3084 KiB / slot
slot_count      =     32
total           = ~ 96 MiB / 128 MiB                     ✓
```
These defaults are chosen from measured latency sweeps rather than from
wire-format arithmetic alone. With Open MPI PML/UCX on this tree, ordinary
`MPI_Send` goes through `mca_pml_ucx_send_nbr()` into `ucp_tag_send_nbx()`,
and UCX defaults `PROTO_ENABLE=y`, so protocol v2 selects between eager short,
eager bcopy single/multi, and rendezvous using its own headers and cost model.
The best reasoning-backed configuration found so far is to keep both geometry
knobs 64-byte aligned, leave `BCOPY_SEG_SIZE=32768` for 32KiB-class raw bcopy
capacity, and reduce `FIFO_ELEM_SIZE` to `16448` so 16KiB-class payloads stay
comfortably on short without over-extending the short window into slower
32KiB-class territory.

When the last local iface on an export exits, UCX resets the entire local
export region to zero before another attach may re-initialize the pool.

---

## FIFO element layout

`uct_obmm_fifo_element_t` (16 bytes, packed):

| field      | bytes | notes                                          |
|------------|-------|------------------------------------------------|
| flags      |   1   | BCOPY bit only                                 |
| am_id      |   1   |                                                |
| length     |   2   | u16 — bytes passed to the AM callback          |
| generation |   4   | receiver-slot generation token at TX time      |
| header     |   8   | am_short user-visible 8B header (unused bcopy) |

For **am_short**, callback data starts at `&elem->header`, so the callback sees
`[header | payload]` and `elem->length = sizeof(header) + payload_length`.
The raw UCT limit remains `max_short = elem_size - 16`.

For **am_bcopy**, payload lives in the paired `desc[N]`. On bcopy the sender
writes:
- `elem->flags  = BCOPY`
- `elem->am_id  = id`
- `elem->length = pack_cb_returned_length`     (≤ seg_size, so ≤ u16max)
- `elem->generation = ep->expected_generation`
- `elem->header = 0` (unused)

Receiver, on seeing FLAG_BCOPY, computes
`desc[N] = slot_descs + (idx & mask) * seg_size` and calls
`uct_iface_invoke_am(am_id, desc[N], length, 0)`.

Because `desc[N]` is bound 1:1 to `elem[N]`, no extra metadata is
exchanged in the FIFO element to locate the desc.

### `length` field width

`elem->length` stays `uint16_t`, capping `seg_size` at 65535. We reject
larger `seg_size` at iface init. If a future `seg_size > 64KiB` is
needed, widen `length` to `uint32_t` and update the compatibility checks.

---

## Sender side (`obmm_ep`)

Each EP binds to one outbound lane in its **local sender-owned slot**:

- slot base = local `iface->slot`
- bank = `local` if the peer device address names our export region, otherwise
  `remote`
- destination lane index = peer `iface_addr.slot_index`

Both `am_short` and `am_bcopy` publish to that lane identically:

```
1. load local cached tail for this lane
2. if (tx_index - cached_tail) >= fifo_size:
        bus_load_fence; refresh cached_tail; recheck;
        if still full: return UCS_ERR_NO_RESOURCE
3. idx = tx_index, N = idx & mask
4. payload write:
      short: memcpy header+payload into elem[N]+1
      bcopy: pack_cb(desc[N], arg) -> length
5. fill elem[N] header fields (flags, am_id, length, generation, [header])
6. ucs_memory_bus_store_fence()                          <- release barrier
7. tx_index++
8. lane_ctl->head = tx_index
```

There is no shared producer lock, shared head CAS, or shared FAA. All hot-path
ownership is single-writer by construction.

### Pending

`ep_pending_add` always queues into the iface arbiter. `iface_progress`
dispatches pending on every call, even if no RX completion occurred, because
depth-1 lanes will hit `UCS_ERR_NO_RESOURCE` frequently.

---

## Receiver side (`obmm_iface_progress`)

```
round-robin over registered inbound lanes, up to fifo_max_poll completions:
    if lane is inactive: continue
    ucs_memory_bus_load_fence()
    if lane_ctl->sender_generation != expected_sender_generation:
        continue                                             (stale registration)
    if lane_ctl->head == rx_index:
        continue                                             (no work)
    elem = elem[rx_index & mask]
    ucs_memory_bus_load_fence()                              (acquire payload)
    if elem->generation != iface->generation:
        drop silently (slot was reused after our death+rebirth)
    elif elem->flags & BCOPY:
        desc = desc[rx_index & mask]
        invoke_am(am_id, desc,  length, 0)
    else:
        invoke_am(am_id, &elem->header, length, 0)
    rx_index++
    full bus fence
    if lane_ctl->sender_generation != expected_sender_generation:
        skip ack publish for this completion
    lane_ctl->tail = rx_index
    ucs_memory_bus_store_fence()
    if lane_ctl->sender_generation != expected_sender_generation:
        skip tail_generation publish
    lane_ctl->tail_generation = expected_sender_generation
```

Each inbound lane is registered by `(bank, sender_slot_index)` and refcounted,
so multiple EPs to the same sender slot do not duplicate-consume one lane.

---

## Capabilities (`iface_query`)

| flag                            | v1 | v2 | notes                       |
|---------------------------------|----|----|-----------------------------|
| AM_SHORT                        |  ✓ |  ✓ | max = elem_size - 16        |
| AM_BCOPY                        |  ✓ |  ✓ | v1: = max_short (cramped); v2: = seg_size |
| PENDING                         |  ✓ |  ✓ | iface arbiter, always queued |
| CONNECT_TO_IFACE                |  ✓ |  ✓ |                             |
| CB_SYNC                         |  ✓ |  ✓ |                             |
| INTER_NODE                      |  ✓ |  ✓ | required for cross-host UCT (otherwise OMPI's NET_ONLY filter strips us — see ucp_worker.c:2962) |

**Not advertised** in v1/v2: PUT/GET (any), ATOMIC, AM_ZCOPY, EP_CHECK,
AM_DUP, ERRHANDLE_PEER. Adding any of these requires a separate design
note.

---

## Wire-format compat

`uct_obmm_iface_addr_t` carries `(slot_index, generation, pid,
fifo_size, fifo_elem_size, bcopy_seg_size)`. Two ifaces are mutually
reachable iff all three geometry fields match — guarded in
`is_reachable_v2`. Pool compatibility is enforced by the shared pool geometry
checks in `pool_attach`/`pool_open`; the shared region does not persist a
separate pool version word or filler replacement field.

---

## Configuration knobs

All under `UCX_OBMM_*` prefix.

| knob                      | default | meaning                          |
|---------------------------|---------|----------------------------------|
| BW                        | 3400MBs | effective transport bandwidth reported to UCP for lane/protocol cost modeling; optional |
| FIFO_SIZE                 |     1   | mailbox depth per sender->receiver lane (power of 2) |
| FIFO_ELEM_SIZE            | 16448   | bytes per FIFO elem (incl. 16B hdr) → raw UCT max_short = 16432 total bytes |
| BCOPY_SEG_SIZE   (v2 NEW) | 32768   | bytes per paired desc → raw UCT max_bcopy |
| FIFO_MAX_POLL             |    16   | RX completions per progress()     |
| MEMIDS        (optional)  |   ""    | comma-separated explicit shmdev memids (for example `1,2`); when set, obmm queries only these memids instead of scanning all shmdevs. Regardless of whether this knob is set, discovery is fail-fast: any discovered/requested shmdev that is missing, unusable, or yields an invalid export/import topology fails md_open |

`BW` is a UCP-facing estimate, not a wire-format limit. UCP folds it into lane
selection and protocol cost modeling, so it should track sustained transport
throughput rather than a one-off peak number. Leaving `UCX_OBMM_BW` unset is
valid; obmm then uses the built-in default above.

Validation at iface init:
- `FIFO_SIZE` > 0, power of 2
- `FIFO_ELEM_SIZE` > sizeof(elem_hdr) and `(elem_size - hdr) <= UINT16_MAX`
- `BCOPY_SEG_SIZE` > 0 and `BCOPY_SEG_SIZE <= UINT16_MAX`
- `slot_count * slot_stride + pool_overhead <= region->length`

---

## Future scope (not in v2)

- `am_zcopy`: requires UCT MD memory-handle plumbing (`mem_reg`,
  `mkey_pack`, `mem_attach`). Currently obmm has no MD-level
  registration — every peer access is via the pre-existing 128 MiB
  region. Out of scope until libobmm-aware md is added.
- `put_bcopy / get_bcopy`: blocked by the same MD plumbing; UCP RMA
  cannot be served by the FIFO-only data path.
- Multi-region per node, NUMA-aware slot placement.
- Variable-size desc allocator (mm-style mpool) to cover medium-size
  messages without burning seg_size per FIFO depth.

---

## Receive-side fence ordering (full bus fence at tail release)

`ucs_memory_bus_store_fence()` on aarch64 is `dmb oshst` — store→store
only. It does NOT order prior loads against subsequent stores. The
receiver's lifetime invariant requires:

```
loads from desc[N]  HAPPENS-BEFORE  store to lane_ctl->tail = N+1
```

Otherwise a sender that observes the new tail can reuse `desc[N]` while the
receiver core still has outstanding loads in flight from the old lap's
payload. We therefore use `uct_obmm_bus_full_fence()` before the tail store,
not only a store-store fence.

Why a local helper rather than adding `ucs_memory_bus_fence()` for all
archs in `ucs/arch/`: AGENTS.md restricts changes to
`ucx/src/uct/obmm/`. If/when the helper proves useful elsewhere, it can
be promoted to ucs/arch — but that is a separate ask.

## Receive-buffer semantics for AM handler

`invoke_am(..., desc, length, 0)` — the trailing `0` (no
`UCT_CB_PARAM_FLAG_DESC`) means the data pointer is valid **only for the
duration of the callback**. The handler MUST NOT retain it; UCP knows
this contract and copies into its own buffer when persistence is
needed.

This is by design: the desc lifetime is bound 1:1 to the FIFO element
slot, so as soon as the receiver advances `tail` past N, the sender is
free to overwrite `desc[N]` on the next lap. There is no separate desc
allocator to support post-callback retention.

If a future need arises to support `UCT_CB_PARAM_FLAG_DESC`-style
returnable descriptors (e.g. for AM zcopy or large-message rendezvous
without copy), it requires a real per-iface mpool of receive
descriptors that the upper layer can hold and release explicitly —
mirroring `uct_mm_recv_desc_t`. Out of scope for v2.

## Verification (no hardware)

Per `.github/skills/ucx-build-verify/SKILL.md`:

1. Build via `task` agent: `./autogen.sh && ./contrib/configure-devel
   && make -j && make install`.
2. `ucx_info -d -t obmm` → confirm `am_short` and `am_bcopy` lines:
   `max_short` should report 16432 raw bytes and `max_bcopy` should reflect
   raw `seg_size` (default 32768).
3. `ucx_info -c | grep OBMM` → confirm `FIFO_SIZE=1` default and
   `BCOPY_SEG_SIZE` entry.
4. `nm -D libuct.so | grep uct_obmm_ep_am_bcopy` → exists.
5. Hardware-required checks (cross-node MPI, sweep sizes through
   `> max_short` and `> max_bcopy`) deferred to user-driven runs on
   the real two-node setup.
