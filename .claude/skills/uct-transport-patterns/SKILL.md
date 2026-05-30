---
name: uct-transport-patterns
description: >
  UCX UCT transport-layer development patterns. Use when implementing or
  modifying any UCT transport (md / iface / ep) — especially the obmm
  transport — to know which existing transports to study and which UCX
  framework macros / hooks must be wired up correctly.
---

# UCT Transport Patterns

UCX UCT is a heavily macro- and hook-table-driven framework. Transport work
should follow UCX framework contracts and study the most relevant in-tree
examples for the capability being changed, rather than inventing structure or
assuming `sm/` transports are the only valid reference. This skill tells the
agent which references to read and which framework contracts must be honored.

## Reference transports (in this repo)

Use the references that match the capability you are touching:

- `ucx/src/uct/sm/mm/base/mm_{md,iface,ep}.{c,h}` — one useful reference for
  FIFO-style AM transports and pending wiring
- `ucx/src/uct/sm/self/` — minimal AM / endpoint wiring reference
- `ucx/src/uct/sm/scopy/base/` — async software-copy quota / arbiter patterns
- `ucx/src/uct/tcp/` — inter-node AM bcopy/zcopy and non-SM transport wiring
- `ucx/src/uct/ib/rc/base/`, `ucx/src/uct/ib/ud/base/` — capability exposure,
  zcopy/RMA/atomic surfaces, and perf modeling examples
- `ucx/src/uct/cuda/`, `ucx/src/uct/rocm/`, `ucx/src/uct/ze/` — copy-style
  transports with transport-specific `iface_query()` / `estimate_perf()`
- `ucx/src/uct/base/uct_iface.h`           — `uct_iface_ops_t`,
                                              `UCT_TL_DEFINE_ENTRY`,
                                              `UCT_SINGLE_TL_INIT`
- `ucx/src/uct/api/uct.h`                  — public ep / iface signatures

The current obmm transport at
`ucx/src/uct/obmm/base/{obmm_md,obmm_iface,obmm_ep}.{c,h}` already uses a
normal md/iface/ep split while implementing its own NC FIFO semantics; new
code must keep that layering coherent.

## Framework contracts that must NOT be broken

When editing UCT transport code, every one of these must remain consistent or
the transport will silently fail to load / register:

1. Component registration
   - `UCT_TL_DEFINE_ENTRY(&uct_obmm_component, obmm, query_tl_devices_fn,
     iface_t, "OBMM_", config_table, config_t)` in `obmm_iface.c`
   - `UCT_SINGLE_TL_INIT(&uct_obmm_component, obmm, ...)` in `obmm_iface.c`
   - `uct_component_t uct_obmm_component = { ... }` in `obmm_md.c`

2. Class hierarchy (UCS_CLASS_*)
   - `uct_obmm_iface_t` derives from `uct_base_iface_t`
   - `uct_obmm_ep_t`    derives from `uct_base_ep_t`
   - INIT / CLEANUP / DEFINE / DEFINE_NEW_FUNC / DEFINE_DELETE_FUNC must all
     be present and matched, or link will fail with `_init`/`_cleanup`
     undefined symbols.

3. ops tables
   - `uct_iface_ops_t`         — set every field; unused fields go to
     `ucs_empty_function_return_unsupported` cast to the right func type.
   - `uct_iface_internal_ops_t` — same.
   - `uct_md_ops_t`            — same.
   - When implementing a new capability (e.g. `ep_am_short`), update BOTH:
     a) the ops-table entry, and
     b) `iface_query` so `attr->cap.flags |= UCT_IFACE_FLAG_AM_SHORT` and
        `attr->cap.am.max_short` is set to a real value.

4. iface_query capability bits
   - The transport will be selected by ucp only if its `cap.flags` and the
     numeric caps (`max_short`, etc.) match what the protocol layer asks for.
   - Current obmm baseline advertises `AM_SHORT | AM_BCOPY | PENDING |
     CONNECT_TO_IFACE | CB_SYNC | INTER_NODE`. When adding a new capability
     (for example `AM_ZCOPY` or PUT/GET), update both the flag bits and the
     corresponding numeric caps in `iface_query()`.

5. Reachability
   - `iface_is_reachable_v2` is what UCP uses; the legacy
     `iface_is_reachable` field stays as `uct_base_iface_is_reachable`.
   - Current obmm reachability matches exporter identity plus wire geometry
     against the MD's mapped export/import regions. Do not regress it to a
     same-host-only `uct_sm_iface_is_reachable` check.

## Current obmm AM data-path pattern

Reference candidates include `uct_mm_ep_am_short`, `uct_mm_ep_am_bcopy`,
`uct_tcp_ep_am_bcopy`, and the corresponding progress / capability code in the
relevant transports, then map those ideas onto obmm's NC FIFO + paired-desc
layout.

The v2 obmm transport has **two distinct send paths**:

**am_short (SPSC fast path, no CAS):**

1. Select deterministic SPSC lane from sender slot index + sender side
   (local 0–31, import 32–63).
2. Lazily activate the lane's bit in the shared active-mask bitmap on first send.
3. Check SPSC ring occupancy (head − cached_tail < lane_fifo_size = 8);
   if full, bus_load_fence + refresh cached_tail + recheck.
4. Write `[header | payload]` inline into the lane element.
5. `ucs_memory_bus_store_fence()` (release).
6. Store `lane->ctl.head = head + 1`.
7. Return `UCS_OK` (or `UCS_ERR_NO_RESOURCE` if lane FIFO is full).

**am_bcopy (legacy shared FIFO, CAS reserve):**

1. Reserve a slot in the peer's shared receive FIFO via CAS on
   `peer_ctl->head` (NOT FAA — an FAA claim that later discovers "full"
   cannot be rolled back). On aarch64 NC this CAS uses explicit LSE
   instructions (`obmm_atomic.h`).
2. `pack_cb` writes payload directly into the paired `desc[N]` area
   (1:1 with FIFO element `N`).
3. Fill `elem[N]` metadata (am_id, length, generation, header=0).
4. `ucs_memory_bus_store_fence()` (release).
5. Publish `elem[N]->flags = OWNER | BCOPY`.

Conceptual flow on the **receiver** side, inside `iface_progress`:

1. Drain active SPSC short lanes first (hot-lane hint + active-mask bitmap),
   copying `[header|payload]` out of NC lane elements into a local bounce
   buffer before invoking the AM handler. Tail is published lazily in
   batches (half the lane depth) with `uct_obmm_bus_full_fence()`.
2. Drain legacy FIFO bcopy metadata up to remaining poll budget: read
   `elem[read_index]`, check owner-bit parity, issue bus_load_fence, validate
   generation, dispatch `desc[N]` via `uct_iface_invoke_am(...)`, advance
   `read_index`.
3. Publish `recv_ctl->tail` with `uct_obmm_bus_full_fence()` (not plain
   `bus_store_fence` — must order prior desc[] loads before the tail store).
4. Dispatch pending queue via `ucs_arbiter_dispatch`.

## Helper macros worth knowing

- `UCT_CHECK_AM_ID(am_id)`  — validate AM id at entry of AM send ops
- `UCT_CHECK_LENGTH(length, 0, max_short, "am_short")`
- `UCT_TL_EP_STAT_OP(ep, AM, SHORT, length)`
- `uct_iface_invoke_am(iface, id, data, len, flags)`
- `ucs_derived_of(p, type)`  — downcast
- `UCS_CLASS_CALL_SUPER_INIT(super_t, ...)`
- `UCS_STATIC_BITMAP_*`, `ucs_arbiter_*`  — relevant to obmm pool metadata and
  pending queue wiring

## Required workflow when touching transport code

1. Before designing a new function, query the vector DB (see
   `vector-db-retrieval` skill) for the closest relevant UCX analogue. Do not
   guess macro signatures or assume the answer must come from `sm/`.
2. Cross-check the chosen reference file with a direct `view` to confirm
   the exact prototype, since UCX revisions may have shifted signatures.
3. After implementation, re-read `iface_query` and the ops tables to make
   sure capability bits and function pointers stay in sync.
