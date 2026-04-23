---
name: uct-transport-patterns
description: >
  UCX UCT transport-layer development patterns. Use when implementing or
  modifying any UCT transport (md / iface / ep) — especially the obmm
  transport — to know which existing transports to mimic and which UCX
  framework macros / hooks must be wired up correctly.
---

# UCT Transport Patterns

UCX UCT is a heavily macro- and hook-table-driven framework. New transports
should be implemented by **mirroring an existing reference transport**, not by
inventing structure. This skill tells the agent which references to read and
which framework contracts must be honored.

## Reference transports (in this repo)

Read these before writing any obmm transport code:

- `ucx/src/uct/sm/mm/base/mm_md.{c,h}`     — shared-memory MD base
- `ucx/src/uct/sm/mm/base/mm_iface.{c,h}`  — FIFO + AM short reception loop
- `ucx/src/uct/sm/mm/base/mm_ep.{c,h}`     — `uct_mm_ep_am_short` reference
- `ucx/src/uct/sm/mm/posix/mm_posix.c`     — concrete mm provider
- `ucx/src/uct/sm/mm/sysv/mm_sysv.c`       — concrete mm provider
- `ucx/src/uct/sm/self/`                   — minimal single-process transport
- `ucx/src/uct/sm/base/sm_iface.{c,h}`     — `uct_sm_iface_t` superclass used
                                              by both mm and obmm
- `ucx/src/uct/base/uct_iface.h`           — `uct_iface_ops_t`,
                                              `UCT_TL_DEFINE_ENTRY`,
                                              `UCT_SINGLE_TL_INIT`
- `ucx/src/uct/api/uct.h`                  — public ep / iface signatures

The obmm skeleton at `ucx/src/uct/obmm/base/{obmm_md,obmm_iface,obmm_ep}.{c,h}`
already follows the mm layout; new code must keep that layering.

## Framework contracts that must NOT be broken

When editing UCT transport code, every one of these must remain consistent or
the transport will silently fail to load / register:

1. Component registration
   - `UCT_TL_DEFINE_ENTRY(&uct_obmm_component, obmm, query_tl_devices_fn,
     iface_t, "OBMM_", config_table, config_t)` in `obmm_iface.c`
   - `UCT_SINGLE_TL_INIT(&uct_obmm_component, obmm, ...)` in `obmm_iface.c`
   - `uct_component_t uct_obmm_component = { ... }` in `obmm_md.c`

2. Class hierarchy (UCS_CLASS_*)
   - `uct_obmm_iface_t` derives from `uct_sm_iface_t`
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
     Currently obmm advertises `CONNECT_TO_IFACE | CB_SYNC | EP_CHECK` and all
     `cap.am.max_*` are 0 — am_short impl MUST raise `max_short` and add
     `UCT_IFACE_FLAG_AM_SHORT`.

5. Reachability
   - `iface_is_reachable_v2` is what UCP uses; the legacy
     `iface_is_reachable` field stays as `uct_base_iface_is_reachable`.
   - obmm's reachability already piggybacks on `uct_sm_iface_is_reachable`
     (same-host check). For two-node obmm, this needs reconsideration; see
     the `obmm-api-and-env` skill.

## AM short pattern (the only semantic to implement now)

Reference: `uct_mm_ep_am_short` in `ucx/src/uct/sm/mm/base/mm_ep.c` and the
matching `uct_mm_iface_progress` reception loop in
`ucx/src/uct/sm/mm/base/mm_iface.c`.

Conceptual flow on the **sender** side:

1. Reserve a slot in the peer's receive FIFO (atomic head increment).
2. Pack `[am_id | header | payload]` into the slot's bcopy area or inline
   slot data.
3. Publish the slot (release-store of the "valid" / sequence flag).
4. Return `UCS_OK` (or `UCS_ERR_NO_RESOURCE` if FIFO is full — UCP will
   retry via pending queue).

Conceptual flow on the **receiver** side, inside `iface_progress`:

1. Read local FIFO tail.
2. If a new slot is published (acquire-load on flag), read am_id + header
   + payload.
3. Dispatch via `uct_iface_invoke_am(&iface->super.super, am_id, data,
   length, flags)`.
4. Advance tail.

For obmm, the FIFO and slot memory live in the **pre-imported peer memory
region** (see `obmm-api-and-env`), accessed via mmap'd virtual addresses, not
via `obmm_export/import` calls at runtime.

## Helper macros worth knowing

- `UCT_CHECK_AM_ID(am_id)`  — validate AM id at entry of am_short
- `UCT_CHECK_LENGTH(length, 0, max_short, "am_short")`
- `UCT_TL_EP_STAT_OP(ep, AM, SHORT, length)`
- `uct_iface_invoke_am(iface, id, data, len, flags)`
- `ucs_derived_of(p, type)`  — downcast
- `UCS_CLASS_CALL_SUPER_INIT(super_t, ...)`
- `UCS_STATIC_BITMAP_*`, `ucs_arbiter_*`  — used by mm pending queue (not
  needed for the first am_short pass unless implementing pending)

## Required workflow when touching transport code

1. Before designing a new function, query the vector DB (see
   `vector-db-retrieval` skill) for the closest mm/self analogue. Do not
   guess macro signatures.
2. Cross-check the chosen reference file with a direct `view` to confirm
   the exact prototype, since UCX revisions may have shifted signatures.
3. After implementation, re-read `iface_query` and the ops tables to make
   sure capability bits and function pointers stay in sync.
