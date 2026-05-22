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
- Cross-host atomic RMW on NC is supported only through explicit arm64 LSE
  instructions. Compiler-default LL/SC atomics are unusable on NC mappings.
  obmm therefore uses explicit LSE CAS for shared control-word updates.
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

## Region layout (v2 + small-short SPSC fast path)

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
|   short_lane_table_hdr (active lane bitmap)           |
|   short_lane[64]  (deterministic SPSC tiny-msg lanes) |
|   fifo_elem[fifo_size]  (elem_size each)              |
|   bcopy_desc[fifo_size] (seg_size each)   <-- NEW v2  |
+-------------------------------------------------------+
| slot[1]: …                                            |
…
```

**Key invariant (v2)**: every legacy FIFO element `elem[N]` (N = `idx & mask`)
has a paired `desc[N]` of `seg_size` bytes in the same slot. Lifetime of
`desc[N]` is **identical** to lifetime of `elem[N]` — both are released
together when the receiver bumps tail past index N, and both are
overwritten together by the sender that claims index N+fifo_size.

This eliminates the cross-host desc free-list problem: there is no
separate desc allocator, no per-desc CAS, and no risk of the desc pool
running dry while FIFO slots remain available. The FIFO already has
exactly the right backpressure semantics.

### Slot stride

The slot now also reserves a fixed small-message SPSC area:

- `short_lane_count = 64`
- `short_lane_fifo_size = 8`
- `short_lane_elem_size = 256`

Lanes are deterministic rather than dynamically allocated:

- indices `0..31` are for local same-node senders (keyed by sender slot index)
- indices `32..63` are for import-side senders from the peer node

This matches the current two-node / `slot_count=32` environment and removes
per-message CAS from the entire supported `am_short` path.

The pool's `slot_count` (compile-time `UCT_OBMM_POOL_SLOT_COUNT = 32`)
**times** `slot_stride` MUST fit in `region->length` (128 MiB minus pool
 header overhead). Defaults are picked to favor short-path coverage over
 maximum local process count; lowering `seg_size` and/or `fifo_size`
 reduces per-slot footprint, but the supported local attach count remains
 the compile-time `slot_count`.

Default budget check:
```
fifo_size       =     64
elem_size       =     64   (legacy FIFO metadata stride only)
seg_size        =  32768   (raw UCT max_bcopy = 32768)
short-lane area =     64 + 64*(64 + 128 + 8*256) = ~ 140 KiB / slot
ctl + slot data = 128 + short-lane area + 64*(64+32768) = ~2192 KiB / slot
slot_count      =     32
total           = ~ 68.5 MiB / 128 MiB                   ✓
```
These defaults are chosen for the current latency-first split:

- `am_short` is intentionally capped at the SPSC lane budget (`248` total
  header+payload bytes)
- anything larger moves directly to `am_bcopy`
- the shared FIFO therefore only needs a compact metadata stride rather than a
  large inline-short payload area

If a user stretches both `FIFO_ELEM_SIZE` and `BCOPY_SEG_SIZE` aggressively,
the pool can still overrun the 128 MiB region and attach will fail with a clear
geometry error. With the current single-path short design there is little
reason to increase `FIFO_ELEM_SIZE` beyond a compact metadata stride.

When the last local iface on an export exits, UCX resets the entire local
export region to zero before another attach may re-initialize the pool.

---

## FIFO element layout

`uct_obmm_fifo_element_t` (16 bytes, packed) is unchanged from v1:

| field      | bytes | notes                                          |
|------------|-------|------------------------------------------------|
| flags      |   1   | OWNER bit + BCOPY bit                          |
| am_id      |   1   |                                                |
| length     |   2   | u16 — short stores `[hdr|payload]` bytes, bcopy stores payload bytes |
| generation |   4   | slot generation token at TX time               |
| header     |   8   | am_short user-visible 8B header (unused bcopy) |

`am_short` now has a single internal path:

1. **small-short SPSC fast path**: total short bytes must fit in the fixed
   `short_lane_elem_size - offsetof(header)` budget (currently 248 bytes).
   The sender uses its deterministic SPSC lane and publishes by advancing the
   lane head — no per-message CAS and no legacy FIFO fallback.

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

`am_bcopy` reserves a legacy FIFO slot:

```
1. load head from peer_ctl->head
2. if (head - cached_tail) >= fifo_size:
       bus_load_fence; refresh cached_tail; recheck;
       if still full: return UCS_ERR_NO_RESOURCE
3. CAS head → head+1 (load+CAS, NOT FAA: an FAA claim that later discovers
   "full" cannot be rolled back, so it would leave a permanent gap stalling
   the in-order receiver)
4. compute idx = head, N = idx & mask
5. payload write:
     bcopy: pack_cb(desc[N], arg) -> length
6. fill elem[N] header fields (am_id, length, generation, header=0)
7. ucs_memory_bus_store_fence()                          <- release barrier
8. elem[N]->flags = OWNER_BIT_FOR_THIS_LAP | (BCOPY if bcopy)
```

The OWNER bit alternates each lap of the ring (see `obmm_iface.c`
`uct_obmm_iface_progress` for why). Since `desc[N]` writes happen
**before** the bus_store_fence, they become visible to the peer at the
same time as the published flags byte.

On the target aarch64 NC environment, this CAS must be an explicit LSE
instruction. Generic compiler-lowered atomics may use LL/SC, which is not
supported on NC mappings and must not be used for shared head/state words.

### Small-short SPSC sender path

For `am_short`, sender and receiver use a fixed SPSC ring:

```
1. choose deterministic lane from sender slot index + sender side
2. use ep-local cached head
3. if head - cached_tail >= short_lane_fifo_size:
       bus_load_fence; refresh cached_tail; recheck;
       if still full: return UCS_ERR_NO_RESOURCE
4. write elem[head & (short_lane_fifo_size - 1)] inline
5. bus_store_fence()
6. lane->ctl.head = head + 1
7. update ep-local cached head
```

This removes the success-path remote CAS from all supported `am_short`
traffic. Messages larger than the SPSC budget are expected to use
`am_bcopy`.

### Pending

`ep_pending_add` keeps the real per-ep arbiter path. When the peer still
looks full after the normal tail refresh, requests queue behind any
older pending work so ordering stays intact; iface progress dispatches
that queue after receive-side tail publication.

---

## Receiver side (`obmm_iface_progress`)

```
1. drain active small-short SPSC lanes up to fifo_max_poll
2. drain legacy FIFO bcopy metadata up to remaining budget
```

Legacy FIFO drain remains:

```
loop up to remaining fifo_max_poll:
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
        treat as invalid wire data (am_short no longer uses legacy FIFO)
    read_index++
if any progress:
    ucs_memory_bus_store_fence()
    recv_ctl->tail = read_index
```

Small-short SPSC receive copies `[header|payload]` out of the NC lane element
before invoking the callback. Receiver-side progress keeps a local tail cache
per lane and publishes `lane->ctl.tail` lazily in batches (currently half the
lane depth) rather than after every consumed message. This intentionally does
not force a publish when a drained lane becomes empty, so latency-oriented
ping-pong traffic can amortize the full fence + NC tail store across several
rounds. Receiver progress also keeps a single hot-lane hint: it first retries
the lane that most recently produced data and only falls back to reading the
shared active bitmap if that hint misses. This removes one NC bitmap load plus
bit-iteration from the steady-state ping-pong receive path without delaying
discovery of newly active lanes on a miss. If the observed shared head ever
moves backwards relative to the local tail cache, the receiver treats that as a
lane reset and resynchronizes from shared tail before continuing. Every actual
tail publication still uses the same full bus-fence ordering rule.

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
| AM_SHORT                        |  ✓ |  ✓ | max = 248 via SPSC lane     |
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
| BW                        | 3400MBs | effective transport bandwidth reported to UCP for lane/protocol cost modeling; optional |
| FIFO_SIZE                 |    64   | ring depth (power of 2)          |
| FIFO_ELEM_SIZE            |    64   | bytes per legacy FIFO elem metadata stride |
| BCOPY_SEG_SIZE   (v2 NEW) | 32768   | bytes per paired desc → raw UCT max_bcopy |
| FIFO_MIN_POLL             |    16   | fixed latency-oriented poll floor |
| FIFO_MAX_POLL             |    16   | fixed latency-oriented poll ceiling by default |
| PENDING_QUOTA             |     1   | pending retries per progress()    |
| SHORT_PERF_STATS          |     n   | dump aggregated 1B am_short timing buckets on cleanup for latency diagnosis |
| MEMIDS        (optional)  |   ""    | comma-separated explicit shmdev memids (for example `1,2`); when set, obmm queries only these memids instead of scanning all shmdevs. Regardless of whether this knob is set, discovery is fail-fast: any discovered/requested shmdev that is missing, unusable, or yields an invalid export/import topology fails md_open |

`BW` is a UCP-facing estimate, not a wire-format limit. UCP folds it into lane
selection and protocol cost modeling, so it should track sustained transport
throughput rather than a one-off peak number. Leaving `UCX_OBMM_BW` unset is
valid; obmm then uses the built-in default above.

Validation at iface init:
- `FIFO_SIZE` > 0, power of 2
- `FIFO_ELEM_SIZE` > sizeof(elem_hdr)
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
2. `ucx_info -d -t obmm` → confirm `am_short` and `am_bcopy` lines:
   `max_short` should report 16432 total bytes and `max_bcopy` should reflect
   raw `seg_size` (default 32768).
3. `ucx_info -c | grep OBMM` → confirm new `BCOPY_SEG_SIZE` entry.
4. `nm -D libuct.so | grep uct_obmm_ep_am_bcopy` → exists.
5. Hardware-required checks (cross-node MPI, sweep sizes through
   `> max_short` and `> max_bcopy`) deferred to user-driven runs on
   the real two-node setup.
