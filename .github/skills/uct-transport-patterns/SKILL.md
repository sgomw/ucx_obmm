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
- `ucx/src/uct/sm/base/sm_iface.{c,h}`     — historical shared-memory
                                              reference; mm still uses
                                              `uct_sm_iface_t`, while current
                                              obmm derives directly from
                                              `uct_base_iface_t`
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
     Current obmm baseline advertises `AM_SHORT | AM_BCOPY | PENDING |
     CONNECT_TO_IFACE | CB_SYNC | INTER_NODE`. Any change to what is
     implemented must keep both the flags and the numeric caps in sync.

5. Reachability
   - `iface_is_reachable_v2` is what UCP uses; the legacy
     `iface_is_reachable` field stays as `uct_base_iface_is_reachable`.
   - obmm's reachability already piggybacks on `uct_sm_iface_is_reachable`
     (same-host check). For two-node obmm, this needs reconsideration; see
     the `obmm-api-and-env` skill.

## Current obmm data-path pattern

Reference: `uct_mm_ep_am_short` in `ucx/src/uct/sm/mm/base/mm_ep.c` and the
matching `uct_mm_iface_progress` reception loop in
`ucx/src/uct/sm/mm/base/mm_iface.c`, but adapt them to the current
**receiver-local sharded atomic FIFO** design rather than mm's single shared
receiver FIFO.

Conceptual flow on the **sender** side:

1. Select the outbound shard in the **peer receiver-owned slot**, keyed by:
   - bank (`local` vs `remote`, derived from exporter identity)
   - shard index (currently `local_slot_index & (shard_count - 1)`)
2. Check `(head - cached_tail) < fifo_size`; if full, return
   `UCS_ERR_NO_RESOURCE` so pending can retry later.
3. Reserve `head -> head + 1` with explicit arm64 LSE CAS on aarch64.
4. Pack either:
   - `am_short`: `[header | payload]` into the lane element body
   - `am_bcopy`: payload into the paired desc entry
5. Fill element metadata (`am_id`, `length`, `generation`, `flags`).
6. Publish by bus-store-fencing the payload/metadata writes, then flipping
   the element OWNER bit for that absolute FIFO index.

Conceptual flow on the **receiver** side, inside `iface_progress`:

1. Poll local `(bank, shard)` queues round-robin.
2. If the current element's OWNER parity matches `rx_index`, acquire-load it.
3. Validate receiver generation, then dispatch via
   `uct_iface_invoke_am(&iface->super, am_id, data, length, flags)`.
4. Advance `tail` with the required full-bus fence.
5. One-way traffic such as collectives is handled by the receiver polling
   its own local slot; no dynamic remote-lane discovery is needed.

For obmm, the mailbox slots live in the pre-mapped export/import regions
discovered by the MD; no `obmm_export/import` calls happen at runtime.

## Helper macros worth knowing

- `UCT_CHECK_AM_ID(am_id)`  — validate AM id at entry of am_short
- `UCT_CHECK_LENGTH(length, 0, max_short, "am_short")`
- `UCT_TL_EP_STAT_OP(ep, AM, SHORT, length)`
- `uct_iface_invoke_am(iface, id, data, len, flags)`
- `ucs_derived_of(p, type)`  — downcast
- `UCS_CLASS_CALL_SUPER_INIT(super_t, ...)`
- `UCS_STATIC_BITMAP_*`, `ucs_arbiter_*`  — pending/queue patterns
- `uct_iface_trace_am(...)`               — align obmm AM tracing with UCT

## Required workflow when touching transport code

1. Before designing a new function, query the vector DB (see
   `vector-db-retrieval` skill) for the closest mm/self analogue. Do not
   guess macro signatures.
2. Cross-check the chosen reference file with a direct `view` to confirm
   the exact prototype, since UCX revisions may have shifted signatures.
3. After implementation, re-read `iface_query`, the ops tables, and the
   current receiver-local atomic FIFO semantics to make sure capability
   bits, function pointers, and progress assumptions stay in sync.
