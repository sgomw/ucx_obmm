# obmm UCT transport — design notes

This file is the **single source of truth** for the on-region wire format
and the data-path semantics of the `obmm` UCT transport. Update it
**before** changing layout, capabilities, or sync rules. AGENTS.md
mandates retrieve-before-recall; this doc is the first thing to grep.

Status legend:
- **v1** = shipped, MPI cross-node smoke tests pass.
- **v2 mailbox baseline** = the previously validated sender-owned mailbox
  transport that passed OSU point-to-point plus collective suites on the
  target hardware.
- **Current in-tree design** = receiver-local sharded atomic FIFO with inline
  `am_short`, paired receiver-owned bulk buffers for `am_bcopy`, and pending
  support. Hardware validation is still pending after this pivot.

---

## Locked-in environment facts

(Mirrors `.github/skills/obmm-api-and-env/SKILL.md`. Do NOT contradict
without re-checking that file.)

- Each node pre-exports **one 128 MiB region**; export/import is done
  outside UCX. UCT must NOT call `obmm_export/import/preimport/...`.
- Data-path mapping is **non-cacheable** (`open(... O_SYNC)` + mmap).
  `obmm_set_ownership` is forbidden and irrelevant.
- Cross-host 64-bit FAA/CAS on the target NC mapping are available only when
  emitted as explicit arm64 **LSE** instructions; compiler-default **LL/SC**
  atomics are not supported on this NC memory. A follow-up LSE-based probe
  confirmed both FAA and CAS are usable. The current FIFO redesign therefore
  uses explicit arm64 LSE CAS for shard-head reservation and must not fall
  back to compiler-builtins on arm64.
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

## Region layout (v3)

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
|   bank[local]: shard[0..shard_count-1]                |
|   bank[remote]: shard[0..shard_count-1]               |
+-------------------------------------------------------+
| slot[1]: …                                            |
…
```

Each slot is **receiver-owned** by exactly one iface. Inside that slot:

- there are two banks: `local` and `remote`
- each bank has `shard_count` FIFO shards
- each shard is MPSC / single-consumer:
  - multiple senders CAS-reserve `head`
  - the slot owner alone advances `tail`

Banking is derived from **sender exporter identity relative to the receiver's
local export region**, not from slot index. This avoids local-slot-index and
remote-slot-index collisions in the fixed two-node topology.

Every shard element `elem[N]` has a paired `desc[N]` in that same shard. Their
lifetime is identical: once the receiver advances `tail` past index `N`, any
sender that later reserves that index on the next lap may reuse both.

### Slot stride

```
shard_stride = align_up(
    sizeof(uct_obmm_fifo_ctl_t) +
    fifo_size * elem_size +
    fifo_size * seg_size,
    cacheline)

slot_stride = align_up(
    2 * shard_count * shard_stride,
    cacheline)
```

The pool's `slot_count` (compile-time `UCT_OBMM_POOL_SLOT_COUNT = 32`)
**times** `slot_stride` MUST fit in `region->length` (128 MiB minus pool
header overhead). Defaults are chosen to keep the current 16KiB-class short
path while turning `am_bcopy` into a receiver-owned sender-push bulk path
large enough to keep 128KiB-class eager traffic single-fragment.

Default budget check:
```
shard_count     =      4
fifo_size       =      2
elem_size       =  16448   (raw UCT max_short = 16424 total bytes)
seg_size        = 229376   (raw UCT max_bcopy = 229376)
shard_stride    = 491776
ctl + slot data = 8 * 491776 = ~3842 KiB / slot
slot_count      =     32
total           = ~120 MiB / 128 MiB                    ✓
```
These defaults are chosen from measured latency sweeps rather than from
wire-format arithmetic alone. With Open MPI PML/UCX on this tree, ordinary
`MPI_Send` goes through `mca_pml_ucx_send_nbr()` into `ucp_tag_send_nbx()`,
and UCX defaults `PROTO_ENABLE=y`, so protocol v2 selects between eager short,
eager bcopy single/multi, and rendezvous using its own headers and cost model.
The current reasoning-backed configuration keeps both geometry knobs 64-byte
aligned, leaves `FIFO_ELEM_SIZE=16448` so 16KiB-class payloads stay
comfortably on short, and repurposes the paired `desc[N]` region as a real
receiver-owned bulk buffer for `am_bcopy`. The key design constraint is to keep
one reservation model: one shard-head CAS claims both `elem[N]` and `desc[N]`.
That avoids introducing a second bulk-credit protocol which would otherwise
need rollback/hole handling if control-FIFO and bulk-buffer reservations
succeeded in different orders. The trade-off is capacity: under the 128 MiB
budget, `4 x 2` leaves only 8 bulk buffers per bank (2 per shard), so pending
pressure rises under heavy many-sender contention even though 128KiB-class
eager sends no longer need the earlier 5-fragment AM multi path.

When the last local iface on an export exits, UCX resets the entire local
export region to zero before another attach may re-initialize the pool.

---

## FIFO element layout

`uct_obmm_fifo_element_t` (24 bytes, naturally aligned):

| field      | bytes | notes                                          |
|------------|-------|------------------------------------------------|
| flags      |   1   | OWNER parity bit + BCOPY bit                   |
| am_id      |   1   |                                                |
| reserved   |   2   | keeps the inline header naturally aligned      |
| generation |   4   | receiver-slot generation token at TX time      |
| length     |   4   | u32 — bytes passed to the AM callback          |
| header     |   8   | am_short user-visible 8B header (unused bcopy) |

For **am_short**, callback data starts at `&elem->header`, so the callback sees
`[header | payload]` and `elem->length = sizeof(header) + payload_length`.
The raw UCT limit remains `max_short = elem_size - 24`.

For **am_bcopy**, payload lives in the paired `desc[N]`. On bcopy the sender
writes:
- `elem->flags  = BCOPY`
- `elem->am_id  = id`
- `elem->length = pack_cb_returned_length`     (≤ seg_size, stored as u32)
- `elem->generation = ep->expected_generation`
- `elem->header = 0` (unused)

Receiver, on seeing FLAG_BCOPY, computes
`desc[N] = slot_descs + (idx & mask) * seg_size` and calls
`uct_iface_invoke_am(am_id, desc[N], length, 0)`.

Because `desc[N]` is bound 1:1 to `elem[N]`, no extra metadata is
exchanged in the FIFO element to locate the desc. This is the receiver-owned
bulk-buffer sender-push protocol: sender copies directly into the receiver slot
bulk buffer, then publishes the small control element. Publish is mm-style:
sender writes payload/metadata first, then bus-store-fences, then flips the
OWNER bit for that absolute ring index.

An empty shard initializes every element with the OWNER bit set, so the initial
`rx_index == 0` observes the ring as **not ready** until a sender publishes the
first real message with OWNER parity matching that absolute index.

### `length` field width

`elem->length` is `uint32_t`, so `seg_size` is no longer capped at 65535.
Compatibility still relies on the explicit layout id plus the geometry fields
carried in `iface_addr`.

---

## Sender side (`obmm_ep`)

Each EP binds to one outbound shard in the **peer receiver-owned slot**:

- slot base = peer `iface_addr.slot_index`
- bank = `local` if the peer device address names our export region, otherwise
  `remote`
- shard = `local_slot_index & (shard_count - 1)`

Both `am_short` and `am_bcopy` publish to that shard identically:

```
1. if this EP already has queued pending requests: return UCS_ERR_NO_RESOURCE
   to preserve order
2. load shard head and cached tail
3. if (head - cached_tail) >= fifo_size:
       bus_load_fence; refresh cached tail; recheck;
       if still full: return UCS_ERR_NO_RESOURCE
4. reserve with explicit-LSE CAS:
       if (CAS(head, head+1) fails) retry from step 2
5. N = head & mask
6. payload write:
      short: memcpy header+payload into elem[N]+1
      bcopy: pack_cb(desc[N], arg) -> length
7. fill elem[N] metadata (am_id, length, generation, [header], BCOPY flag)
8. ucs_memory_bus_store_fence()                          <- release barrier
9. flip OWNER parity bit in elem[N].flags                <- publish
```

There is no separate shared lock word. The only hot-path cross-node atomic is
the shard-head CAS.

### Pending

`ep_pending_add` queues into the iface arbiter. `iface_progress` dispatches
pending on every call so senders blocked on shard capacity make forward
progress as soon as the local receiver releases tail.

---

## Receiver side (`obmm_iface_progress`)

```
round-robin over local (bank, shard) queues, up to fifo_max_poll completions:
    elem = elem[rx_index & mask]
    if OWNER parity != expected parity for rx_index:
        continue                                             (no work yet)
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
    shard_ctl->tail = rx_index
    ucs_memory_bus_store_fence()
```

Because the receive queues are local again, **one-way collectives no longer
need dynamic unregistered-lane discovery**. Progress only depends on polling
the local receiver slot.

### Unsupported peer-failure window

If a sender wins the shard-head CAS and dies before publishing the OWNER bit,
that shard can stall until peer-failure handling exists. obmm still does not
advertise `EP_CHECK` or `ERRHANDLE_PEER`; this is treated as an unsupported
peer-failure case rather than a supported recovery path.

---

## Capabilities (`iface_query`)

| flag                            | v1 | v2 | notes                       |
|---------------------------------|----|----|-----------------------------|
| AM_SHORT                        |  ✓ |  ✓ | max = elem_size - 24        |
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

`uct_obmm_iface_addr_t` carries `(slot_index, generation, pid, layout,
shard_count, fifo_size, fifo_elem_size, bcopy_seg_size)`. Two ifaces are
mutually reachable iff:

- `layout == UCT_OBMM_FIFO_LAYOUT_BULK_SHARDED`
- `shard_count`, `fifo_size`, `fifo_elem_size`, and `bcopy_seg_size` match

Pool compatibility is enforced by the shared pool geometry checks in
`pool_attach`/`pool_open`; the shared region does not persist a separate pool
version word or filler replacement field.

---

## Configuration knobs

All under `UCX_OBMM_*` prefix.

| knob                     | default | meaning                          |
|--------------------------|---------|----------------------------------|
| BW                       | 3400MBs | effective transport bandwidth reported to UCP for lane/protocol cost modeling; optional |
| SHARD_COUNT              |     4   | receive FIFO shards per bank (power of 2) |
| FIFO_SIZE                |     2   | depth per shard (power of 2) |
| FIFO_ELEM_SIZE           | 16448   | bytes per FIFO elem (incl. 24B hdr) → raw UCT max_short = 16424 total bytes |
| BCOPY_SEG_SIZE           | 229376  | bytes per paired receiver-owned bulk buffer → raw UCT max_bcopy |
| FIFO_MAX_POLL            |    16   | RX completions per progress()     |
| MEMIDS       (optional)  |   ""    | comma-separated explicit shmdev memids (for example `1,2`); when set, obmm queries only these memids instead of scanning all shmdevs. Regardless of whether this knob is set, discovery is fail-fast: any discovered/requested shmdev that is missing, unusable, or yields an invalid export/import topology fails md_open |

`BW` is a UCP-facing estimate, not a wire-format limit. UCP folds it into lane
selection and protocol cost modeling, so it should track sustained transport
throughput rather than a one-off peak number. Leaving `UCX_OBMM_BW` unset is
valid; obmm then uses the built-in default above.

Validation at iface init:
- `SHARD_COUNT` > 0, power of 2, `<= 32`
- `FIFO_SIZE` > 0, power of 2
- `FIFO_ELEM_SIZE` > sizeof(elem_hdr)
- `BCOPY_SEG_SIZE` > 0
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
loads from desc[N]  HAPPENS-BEFORE  store to shard_ctl->tail = N+1
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
   `max_short` should report 16424 raw bytes and `max_bcopy` should reflect
   raw `seg_size` (default 229376).
3. `ucx_info -c | grep OBMM` → confirm `SHARD_COUNT=4`, `FIFO_SIZE=2`,
   and `BCOPY_SEG_SIZE` entries.
4. `nm -D libuct.so | grep uct_obmm_ep_am_bcopy` → exists.
5. Hardware-required checks (cross-node MPI, sweep sizes through
   `> max_short` and `> max_bcopy`) deferred to user-driven runs on
   the real two-node setup.
