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
- Cross-host atomic FAA/CAS on NC is supported (project-owner statement).
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
|   uct_obmm_fifo_ctl_t  (head + tail, padded)          |
|   fifo_elem[fifo_size]  (elem_size each)              |
|   bcopy_desc[fifo_size] (seg_size each)   <-- NEW v2  |
+-------------------------------------------------------+
| slot[1]: …                                            |
…
```

**Key invariant (v2)**: every FIFO element `elem[N]` (N = `idx & mask`)
has a paired `desc[N]` of `seg_size` bytes in the same slot. Lifetime of
`desc[N]` is **identical** to lifetime of `elem[N]` — both are released
together when the receiver bumps tail past index N, and both are
overwritten together by the sender that claims index N+fifo_size.

This eliminates the cross-host desc free-list problem: there is no
separate desc allocator, no per-desc CAS, and no risk of the desc pool
running dry while FIFO slots remain available. The FIFO already has
exactly the right backpressure semantics.

### Slot stride

```
slot_stride = align_up(
    sizeof(uct_obmm_fifo_ctl_t) +
    fifo_size * elem_size +
    fifo_size * seg_size,
    cacheline)
```

The pool's `slot_count` (compile-time `UCT_OBMM_POOL_SLOT_COUNT = 256`)
**times** `slot_stride` MUST fit in `region->length` (128 MiB minus pool
header overhead). Defaults are picked so that this holds; callers may
shrink (not grow) `seg_size` / `fifo_size` if they need more slots.

Default budget check:
```
fifo_size       =     64
elem_size       =   2048   (max_short = 2032)
seg_size        =   4096   (max_bcopy = 4096)
ctl + slot data = 128 + 64*(2048+4096) = ~384 KiB / slot
slot_count      =    256
total           = ~ 96 MiB / 128 MiB                     ✓
```
If a user bumps `seg_size` to 8192, total grows to ~160 MiB and pool
attach fails with a clear error pointing at the geometry knobs.

When the last local iface on an export exits, UCX resets the entire local
export region to zero before another attach may re-initialize the pool.

---

## FIFO element layout

`uct_obmm_fifo_element_t` (16 bytes, packed) is unchanged from v1:

| field      | bytes | notes                                          |
|------------|-------|------------------------------------------------|
| flags      |   1   | OWNER bit + BCOPY bit                          |
| am_id      |   1   |                                                |
| length     |   2   | u16 — payload bytes (excl. am_short hdr)       |
| generation |   4   | slot generation token at TX time               |
| header     |   8   | am_short user-visible 8B header (unused bcopy) |

**am_short payload** still lives inline at `elem + 1` in the FIFO area.
Wire format and `max_short = elem_size - 16` are unchanged.

**am_bcopy payload** (NEW in v2) lives in the paired `desc[N]` instead
of inside the FIFO element body. The element body is unused for bcopy;
on bcopy the sender writes:
- `elem->flags  = OWNER | BCOPY`
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

Both `am_short` and `am_bcopy` reserve a slot identically:

```
1. load head from peer_ctl->head
2. if (head - cached_tail) >= fifo_size:
       bus_load_fence; refresh cached_tail; recheck;
       if still full: return UCS_ERR_NO_RESOURCE
3. claim producer lock (`peer_ctl->lock`) with a unique token, then
   store head → head+1, then release the lock
4. compute idx = head, N = idx & mask
5. payload write:
     short: memcpy header+payload into elem[N]+1
     bcopy: pack_cb(desc[N], arg) -> length
6. fill elem[N] header fields (am_id, length, generation, [header])
7. ucs_memory_bus_store_fence()                          <- release barrier
8. elem[N]->flags = OWNER_BIT_FOR_THIS_LAP | (BCOPY if bcopy)
```

The OWNER bit alternates each lap of the ring (see `obmm_iface.c`
`uct_obmm_iface_progress` for why). Since `desc[N]` writes happen
**before** the bus_store_fence, they become visible to the peer at the
same time as the published flags byte.

The producer lock exists because on the validated target aarch64 NC
environment, cross-node CAS updates memory correctly but its return value
is not reliable enough to use as a head-claim ownership result. The token
lock converts reservation into: acquire lock by readback, re-check space,
plain-store new head, release lock.

### Pending

`ep_pending_add` returns `UCS_ERR_BUSY` (UCP retries via its own
progress loop). Real arbiter is out of scope for v1/v2 — backpressure
is bounded because the receiver drains continuously.

---

## Receiver side (`obmm_iface_progress`)

```
loop up to fifo_max_poll:
    elem = elem[read_index & mask]
    expected_owner = lap_parity(read_index)
    if (elem->flags & OWNER) != expected_owner: break       (no work)
    ucs_memory_bus_load_fence()                              (acquire)
    if elem->generation != iface->generation:
        drop silently (slot was reused after our death+rebirth)
    elif elem->flags & BCOPY:
        desc = desc[read_index & mask]
        invoke_am(am_id, desc,  length, 0)
    else:
        invoke_am(am_id, &elem->header, length, 0)
    read_index++
if any progress:
    ucs_memory_bus_store_fence()
    recv_ctl->tail = read_index
```

**Why the fence ordering is correct for bcopy too**: the
`ucs_memory_bus_load_fence()` issued after observing the flags byte
orders **all** subsequent loads in this iteration — including the
load of `desc[N]` performed inside the AM handler's memcpy. The handler
is `CB_SYNC` (synchronous), so the fence-acquire pairs with the
sender's `bus_store_fence` before publishing flags.

---

## Capabilities (`iface_query`)

| flag                            | v1 | v2 | notes                       |
|---------------------------------|----|----|-----------------------------|
| AM_SHORT                        |  ✓ |  ✓ | max = elem_size - 16        |
| AM_BCOPY                        |  ✓ |  ✓ | v1: = max_short (cramped); v2: = seg_size |
| PENDING                         |  ✓ |  ✓ | returns BUSY only           |
| CONNECT_TO_IFACE                |  ✓ |  ✓ |                             |
| CB_SYNC                         |  ✓ |  ✓ |                             |
| INTER_NODE                      |  ✓ |  ✓ | required for cross-host UCT (otherwise OMPI's NET_ONLY filter strips us — see ucp_worker.c:2962) |

**Not advertised** in v1/v2: PUT/GET (any), ATOMIC, AM_ZCOPY, EP_CHECK,
AM_DUP, ERRHANDLE_PEER. Adding any of these requires a separate design
note.

---

## Wire-format compat

`uct_obmm_iface_addr_t` carries `(slot_index, generation, pid,
fifo_size, fifo_elem_size, bcopy_seg_size)`. v2 **adds** `bcopy_seg_size`
(replaces v1's `reserved` u32 → no struct-size change). Two ifaces are
mutually reachable iff all three geometry fields match — guarded in
`is_reachable_v2`. Pool compatibility is enforced by the shared pool
geometry checks in `pool_attach`/`pool_open`; the shared region does not
persist a separate pool version word or filler replacement field.

---

## Configuration knobs

All under `UCX_OBMM_*` prefix.

| knob                      | default | meaning                          |
|---------------------------|---------|----------------------------------|
| FIFO_SIZE                 |    64   | ring depth (power of 2)          |
| FIFO_ELEM_SIZE            |  2048   | bytes per FIFO elem (incl. 16B hdr) → max_short = 2032 |
| BCOPY_SEG_SIZE   (v2 NEW) |  4096   | bytes per paired desc → max_bcopy |
| FIFO_MAX_POLL             |    16   | RX completions per progress()     |
| MEMIDS        (optional)  |   ""    | comma-separated explicit shmdev memids (for example `1,2`); when set, obmm queries only these memids instead of scanning all shmdevs. Regardless of whether this knob is set, discovery is fail-fast: any discovered/requested shmdev that is missing, unusable, or yields an invalid export/import topology fails md_open |

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
- Real pending arbiter: needed only if profiling shows BUSY-retry
  storms.
- Multi-region per node, NUMA-aware slot placement.
- Variable-size desc allocator (mm-style mpool) to cover medium-size
  messages without burning seg_size per FIFO depth.

---

## Receive-side fence ordering (full bus fence at tail release)

`ucs_memory_bus_store_fence()` on aarch64 is `dmb oshst` — store→store
only. It does NOT order prior loads against subsequent stores. The
receiver's lifetime invariant requires:

```
loads from desc[N]  HAPPENS-BEFORE  store to recv_ctl->tail = N+1
```

Otherwise a sender that observes the new tail can reuse `desc[N]` while
the receiver core still has outstanding loads in flight from the old
lap's payload. We therefore use `uct_obmm_bus_full_fence()` (defined
locally in `obmm_fifo.h`: `dmb osh` on aarch64, `mfence` on x86,
`sync` on ppc64, `fence iorw,iorw` on rv64) before the tail store, not
the bus_store fence.

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
2. `ucx_info -d -t obmm` → confirm `am_short` and `am_bcopy` lines, and
   max_bcopy reflects `seg_size` (default 4096).
3. `ucx_info -c | grep OBMM` → confirm new `BCOPY_SEG_SIZE` entry.
4. `nm -D libuct.so | grep uct_obmm_ep_am_bcopy` → exists.
5. Hardware-required checks (cross-node MPI, sweep sizes through
   `> max_short` and `> max_bcopy`) deferred to user-driven runs on
   the real two-node setup.
