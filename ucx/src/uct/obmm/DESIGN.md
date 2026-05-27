# obmm UCT transport — design notes

This file is the **single source of truth** for the on-region wire format
and the data-path semantics of the current obmm UCT transport family. Update it
**before** changing layout, capabilities, or sync rules. AGENTS.md
mandates retrieve-before-recall; this doc is the first thing to grep.

Status legend:
- **v1** = historical NC-only eager baseline.
- **v2** = earlier split-role baseline (`obmm_nc` + `obmm_cc`).
- **v3** = reverted unified public TL (`obmm`) experiment.
- **v4** = current split-role model with hard locality partitioning.

---

## Locked-in environment facts

(Mirrors `.github/skills/obmm-api-and-env/SKILL.md`. Do NOT contradict
without re-checking that file.)

- Export/import is done outside UCX. UCT must NOT call
  `obmm_export/import/preimport/...`.
- The current in-tree transport uses **two mapping classes**:
  - NC (`open(... O_SYNC)` + mmap) for `obmm_nc` cross-node eager/control
    traffic
  - CC (`open(... O_RDWR)` + mmap) for `obmm_cc` same-node eager traffic
    and for shared bulk-window storage
- `obmm_set_ownership` remains forbidden on the NC eager path, but is a real
  transport dependency for the CC bulk-window path.
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

## Current transport plan

Measured probe data still drives the design:

- same-node CC is effectively as fast as posix
- cross-node per-message CC is far too slow because ownership release dominates
- cross-node bulk CC becomes worthwhile only when ownership is amortized over
  multi-MiB windows

The current implementation again exposes **two public TLs** with hard
reachability partitioning:

1. **`obmm_cc`**
   - same-node-only public TL
   - mapping mode: CC (plain `O_RDWR`) for eager traffic
   - advertises `AM_SHORT`, `AM_BCOPY`, `PENDING`, `CONNECT_TO_IFACE`,
     `CB_SYNC`
   - current phase-1 baseline restores a b4-style same-node CC eager ring with
     config-driven FIFO / desc geometry
   - `am_bcopy` is eager-only on this TL and direct-packs into the paired
     `desc[N]` entry for the claimed FIFO element rather than using the later
     receiver-owned desc-pool / `UCT_CB_PARAM_FLAG_DESC` experiment
2. **`obmm_nc`**
   - cross-node-only public TL
   - mapping mode: NC (`O_SYNC`) for eager/control traffic
   - advertises `AM_SHORT`, `AM_BCOPY`, `PENDING`, `CONNECT_TO_IFACE`,
     `CB_SYNC`, `INTER_NODE`
   - keeps the long-running NC eager path as the validated correctness baseline
3. **Shared CC bulk-window machinery**
   - still transport-internal
   - backed by CC memory after the eager prefix
   - currently used only by `obmm_nc`; it is no longer on the same-node
     `obmm_cc` hot path

The user-visible capability surface stays AM-only. The current design keeps the
two-TL split not to mix short/bcopy lanes for one peer, but to give UCP
separate per-peer capability surfaces and cost models for same-node vs
cross-node peers while preserving a stable user-facing
`UCX_TLS=obmm_cc,obmm_nc,self` configuration.

### Memory budget per node

Approved starting budget:

- **NC region**: 16 MiB
- **CC local/eager prefix**: 38 MiB with the current eager geometry
  (`38396160` bytes rounded up to the 2 MiB bulk-window alignment)
- **CC bulk**: about 506 MiB within the approved 544 MiB CC budget
- **total CC**: 544 MiB
- **total obmm mapped budget**: 560 MiB

### MD / config model

One obmm MD discovers and maps **two independent memid groups**:

- `UCX_OBMM_NC_MEMIDS`: NC shmdevs for `obmm_nc` eager/control traffic
- `UCX_OBMM_CC_MEMIDS`: CC shmdevs for `obmm_cc` eager and shared bulk data
  windows

Both memid groups are required. The old single-list `UCX_OBMM_MEMIDS`
fallback is intentionally removed so the rollout never silently guesses the
wrong region set.

For CC mappings, export regions are opened read/write, while import regions are
initially mapped `PROT_NONE`. Same-node eager uses the local export mapping
only; the bulk path explicitly flips ownership on CC import/export windows only
when the peer is remote.

### Current single-CC-region split

The current staged hybrid implementation assumes the user-approved
single-region CC layout:

- bytes `[0, cc_local_prefix)`   → reserved for the `obmm_cc` eager pool
- bytes `[cc_local_prefix, end)` → reserved for shared sender-owned bulk windows

The later built-in `FIFO_SIZE=8` / `BCOPY_SEG_SIZE=65600` receiver-owned
desc-pool layout is no longer the active same-node baseline. Current phase-1
`obmm_cc` work instead follows the earlier b4-style design: the CC eager pool
uses the normal FIFO + paired-desc slot layout and derives its geometry from
the role's configured `FIFO_SIZE`, `FIFO_ELEM_SIZE`, and `BCOPY_SEG_SIZE`.

### Unified routing policy

The unified TL routes internally instead of relying on UCP multi-TL lane
selection:

1. `am_short`
   - same-node peer → CC eager short lane
   - remote peer    → NC eager short lane
2. `am_bcopy`
   - pack into a free local CC bulk window
   - if the packed length fits the selected eager path's `bcopy_seg_size`,
     copy into the eager desc and publish as eager
   - otherwise publish the CC window through the NC bulk-control slot

This means the current `am_bcopy` implementation still needs a free local bulk
window even when the message later falls back to eager. That is a deliberate
current trade-off of the unified in-progress implementation, not a protocol
guarantee.

### Bulk protocol (current staged implementation)

The bulk path does **not** reuse the eager FIFO payload path. Instead:

1. Every iface allocates one slot from the shared **NC** pool for bulk control.
   That slot carries a bulk-control header plus `window_count` descriptors
   rather than FIFO elements. The header includes the sender slot `generation`,
   so a stale EP cannot consume traffic after that NC pool slot is recycled.
2. Each sender process exposes `window_count` fixed CC windows of
   `window_size` bytes in its own CC export region, after `cc_local_prefix`.
3. For inter-node peers, the sender executes
   `obmm_set_ownership(..., PROT_READ)` before publishing a bulk descriptor. For
   same-node peers it skips ownership and reuses the same window protocol
   directly.
4. The sender writes one NC descriptor
   `(seq, am_id, flags, length, target_slot_index, target_generation, cc_memid,
   sender_generation, ack_generation)` and publishes `req_seq`.
5. On the receive side, every connected EP polls the peer's NC bulk-control
   slot, selects the oldest descriptor targeting its local
   `(slot_index, generation)`, acquires `PROT_READ` only when required,
   invokes the AM callback directly on the CC window payload, releases the
   window back to `PROT_NONE` only for the inter-node case, and publishes
   `ack_seq`.
6. The sender later observes `ack_seq == seq`, reacquires `PROT_WRITE` only for
   ownership-flipped inter-node windows, clears the descriptor, and returns the
   window to the shared local free pool. Both RX selection and reclaim are
   generation-aware so a stale receiver cannot ACK a recycled sender slot's new
   descriptor.

The bulk wire format is explicitly versioned. `iface_addr` carries
`bulk_data_offset`, and each bulk control header carries `version`, so peers
that disagree on the CC split point or ownership-flag semantics are rejected
before they can trust the descriptor layout.

This gives one sender-owned bulk arena per process, shared across all remote
peers, while keeping ordering/doorbells on the NC control plane.

---

## Region layout (v2 + SPSC short fast path)

Inside the exported region:

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
|   short_lane[64]  (deterministic SPSC short lanes)    |
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

The slot now reserves one fixed SPSC short area:

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
short-lane area =     64 + 64*(64 + 128 + 8*256) = ~140 KiB / slot
ctl + slot data = 128 + short-lane area + 64*(64+32768) = ~2192 KiB / slot
slot_count      =     32
total           = ~ 68.5 MiB / 128 MiB                   ✓
```
These defaults are chosen for the current latency-first split:

- `am_short` is intentionally capped at the SPSC lane budget (`246` total
  header+payload bytes)
- anything larger moves directly to `am_bcopy`
- the shared FIFO therefore only needs a compact metadata stride rather than a
  large inline-short payload area

If a user stretches both `FIFO_ELEM_SIZE` and `BCOPY_SEG_SIZE` aggressively,
the pool can still overrun the 128 MiB region and attach will fail with a clear
geometry error. With the current single-path short design there is little
reason to increase `FIFO_ELEM_SIZE` beyond a compact metadata stride.

When the last local iface on an export exits, UCX first scavenges any stale
slot records left by dead processes and then resets the entire local export
region to zero before another attach may re-initialize the pool.

For the current split-role model, the bulk-control slot is still borrowed from
the shared NC pool geometry. Only the first
`uct_obmm_bulk_ctrl_size(window_count)` bytes of that slot are live protocol
state; the rest of the slot is unused padding.

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
   `short_lane_elem_size - offsetof(header)` budget (currently 246 bytes).
   The sender uses its deterministic SPSC lane and publishes by advancing the
   lane head — no per-message CAS and no legacy FIFO fallback.

**am_bcopy payload** (NEW in v2) lives in the desc area instead of inside the
FIFO element body. The element body is unused for bcopy; on bcopy the sender
writes:
- `elem->flags  = OWNER | BCOPY`
- `elem->am_id  = id`
- `elem->length = pack_cb_returned_length`     (≤ seg_size, stored as u32)
- `elem->generation = ep->expected_generation`
- `elem->header = 0` on the current same-node `obmm_cc` fast path; both TLs
  use the direct paired `desc[N]` mapping keyed by the FIFO ring index.

On the current phase-1 `obmm_cc` path, there is no separate receiver-owned
desc allocator. The sender packs directly into `desc[head & mask]`, and the
receiver invokes the AM callback on that shared desc buffer before advancing
tail, matching the older b4-style same-node implementation.

### `length` field width

`elem->length` is now `uint32_t`. This is an intentional active-branch wire
change: the previous `uint16_t` field forced `obmm_cc` to keep
`seg_size <= 65535`, which in turn made common 128 KiB eager traffic spill to
3 fragments instead of 2. During development we assume peers run the same build
and do not add extra compatibility bookkeeping for discarded intermediate
variants.

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

For `am_short`, sender and receiver use fixed SPSC rings:

```
1. choose deterministic lane from sender slot index + sender side
2. if lane meta does not match current sender/receiver identities, refresh
   `meta.{sender_slot_index,sender_generation,sender_pid}`
   and reset `ctl.{head,tail}=0`
3. lazily mark the lane's active-mask bit on the first send from this ep
4. use ep-local cached head
5. if head - cached_tail >= lane_fifo_size:
        bus_load_fence; refresh cached_tail; recheck;
        if still full: return UCS_ERR_NO_RESOURCE
6. write elem[head & (lane_fifo_size - 1)] inline
7. bus_store_fence()
8. lane->ctl.head = head + 1
9. update ep-local cached head
```

This removes the success-path remote CAS from all supported `am_short`
traffic. Messages larger than the SPSC budget are expected to use `am_bcopy`.

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
per lane
and publishes `lane->ctl.tail` lazily in batches (currently half the lane
depth) rather than after every consumed message. This intentionally does not
force a publish when a drained lane becomes empty, so latency-oriented
ping-pong traffic can amortize the full fence + NC tail store across several
rounds. Receiver progress also keeps a single hot-lane hint: it first retries
the lane that most recently produced data and only falls back to reading the
shared active bitmap if the hint misses. If short-lane progress found work and there
is no pending arbiter work plus no legacy FIFO backlog (`recv_ctl->head ==
read_index`), iface progress returns immediately instead of doing an empty
legacy FIFO poll and no-op pending dispatch. If the observed shared head ever
moves backwards relative to the local tail cache, the receiver treats that as a
lane reset and clears the hot-lane hint before continuing. Short-lane
stale-data protection remains per-message: the sender stamps each element with
the receiver slot generation, and receiver progress drops entries whose
`elem->generation` no longer matches `iface->generation`. The acquire-side bus
load fence is only needed after observing `head != tail` and before
dereferencing the element body; the initial control-word `head` check itself
does not need a separate fence. Every actual tail publication still uses the
same full bus-fence ordering rule.

**Why the fence ordering is correct for bcopy too**: the
`ucs_memory_bus_load_fence()` issued after observing the flags byte
orders **all** subsequent loads in this iteration — including the
load of `desc[N]` performed inside the AM handler's memcpy. The handler
is `CB_SYNC` (synchronous), so the fence-acquire pairs with the
sender's `bus_store_fence` before publishing flags.

---

## Capabilities (`iface_query`)

| flag             | v1 | v3 | notes |
|------------------|----|----|-------|
| AM_SHORT         | ✓  | ✓  | max = 246 via SPSC lane |
| AM_BCOPY         | ✓  | ✓  | `obmm_cc` reports CC eager seg size and stays eager-only; `obmm_nc` still reports bulk-window-sized bcopy |
| PENDING          | ✓  | ✓  | per-ep arbiter |
| CONNECT_TO_IFACE | ✓  | ✓  | |
| CB_SYNC          | ✓  | ✓  | |
| INTER_NODE       | ✓  | ✓  | still required for cross-host UCT reachability |

**Not advertised** in v1/v2: PUT/GET (any), ATOMIC, AM_ZCOPY, EP_CHECK,
AM_DUP, ERRHANDLE_PEER. Adding any of these requires a separate design
note.

---

## Wire-format compat

`uct_obmm_device_addr_t` carries the peer process's NC/shared exporter
identity `(exporter_dcna, exporter_deid)`.

`uct_obmm_iface_addr_t` now carries:

- capability `flags`
- NC eager slot geometry
- CC exporter identity plus CC eager slot identity
- NC bulk-control slot identity
- CC bulk-window layout (`bulk_window_count`, `bulk_data_offset`,
  `bulk_window_size`, `bulk_cc_memid`)

This is an intentional wire-format break from the old role-based address
format. The packed worker address still stays within the legacy UCP v1
worker-address packing limits, so `ucp_worker_query()` does not require
`UCX_ADDRESS_VERSION=v2`. During active development, peers are expected to run
the same build rather than negotiate discarded intermediate variants.
Peers must agree on the NC eager geometry and bulk layout, and
`is_reachable_v2` rejects mismatches before `ep_create`. Pool compatibility is
still enforced by the shared pool geometry checks in `pool_attach`/`pool_open`;
the shared region does not persist a separate pool version word or filler
replacement field.

---

## Configuration knobs

- MD-level region selection remains under `UCX_OBMM_*`:
  `UCX_OBMM_NC_MEMIDS`, `UCX_OBMM_CC_MEMIDS`
- TL-level performance / geometry knobs now all use the unified `UCX_OBMM_*`
  prefix.

| knob | default | meaning |
|------|---------|---------|
| BW | 3400MBs | effective transport bandwidth reported to UCP for cost modeling |
| FIFO_SIZE | 64 | ring depth (power of 2) for the NC eager/control pool |
| FIFO_ELEM_SIZE | 64 | bytes per legacy FIFO elem metadata stride |
| BCOPY_SEG_SIZE | 32768 | bytes per paired desc on the NC eager path |
| WINDOW_SIZE | 2m | bytes per CC bulk window / ownership epoch |
| WINDOW_COUNT | 8 | number of CC bulk windows shared by one iface |
| FIFO_MIN_POLL | 16 | fixed latency-oriented poll floor |
| FIFO_MAX_POLL | 16 | fixed latency-oriented poll ceiling by default |
| PENDING_QUOTA | 1 | pending retries per progress() |
| NC_MEMIDS | "" | required comma-separated NC shmdev memids |
| CC_MEMIDS | "" | required comma-separated CC shmdev memids |

`BW` is a UCP-facing estimate, not a wire-format limit. UCP folds it into lane
selection and protocol cost modeling, so it should track sustained transport
throughput rather than a one-off peak number. Leaving `UCX_OBMM_BW` unset is
valid; obmm then uses the built-in default above.

The CC eager path does not expose its own FIFO/bcopy geometry knobs. Its local
pool geometry is fixed to a built-in eager layout (`FIFO_SIZE=8`,
`FIFO_ELEM_SIZE=64`, `BCOPY_SEG_SIZE=65600`, `DESC_COUNT=16`,
`DESC_PREFIX=128`) and the CC-local prefix size is derived from that compiled
layout rather than from separate config keys.

Validation at iface init:
- `FIFO_SIZE` > 0, power of 2
- `FIFO_ELEM_SIZE` > sizeof(elem_hdr)
- `BCOPY_SEG_SIZE` > 0 and `BCOPY_SEG_SIZE <= UINT16_MAX` for the configurable
  NC eager geometry; the built-in `obmm_cc` eager segment is fixed at `65600`
  and relies on the widened 32-bit FIFO `length` field
- `WINDOW_SIZE` > 0 and aligned to the bulk-window alignment
- `WINDOW_COUNT` > 0
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
2. `ucx_info -d -t obmm_cc` / `ucx_info -d -t obmm_nc` → confirm `am_short`
   and `am_bcopy` are both exposed. `max_short` should report the SPSC
   short-lane budget (246 total header+payload bytes); `obmm_cc.max_bcopy`
   should match the CC eager segment size, while `obmm_nc.max_bcopy` should
   match the configured bulk window size.
3. `ucx_info -c | grep OBMM` → confirm `OBMM_NC_MEMIDS`, `OBMM_CC_MEMIDS`, and
   unified `OBMM_*` transport config entries are exposed.
4. `nm -D libuct.so | grep uct_obmm_ep_am_bcopy` → exists.
5. `nm -D libuct.so | grep UCT_TL_NAME\\(obmm\\)` or an equivalent symbol grep
   → confirms the unified TL registration exists.
6. Hardware-required checks (cross-node MPI, sweep sizes through
   `> max_short` and `> max_bcopy`) deferred to user-driven runs on
   the real two-node setup.
