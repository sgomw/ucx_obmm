---
name: uct-transport-patterns
description: >
  UCX UCT transport-layer development patterns. Use when implementing or
  modifying any UCT transport (md / iface / ep) — especially the obmm
  transport — to know which UCX framework macros / hooks must be wired up
  correctly and where optional prior art lives.
---

# UCT Transport Patterns

UCX UCT is a heavily macro- and hook-table-driven framework. Transport work
should follow UCX framework contracts. When useful, in-tree transports can be
consulted as optional prior art for specific capability surfaces, but obmm
performance work does not need to mechanically mirror another transport or
assume `sm/` is the only valid reference. This skill focuses on framework
contracts first and lists a few code locations that may still be useful to
inspect.

## Optional reference points (in this repo)

If you want prior art for a specific capability, these are useful places to
inspect:

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

If you want prior art for AM send/progress wiring, useful examples include
`uct_mm_ep_am_short`, `uct_mm_ep_am_bcopy`, `uct_tcp_ep_am_bcopy`, and the
corresponding progress / capability code in those transports. Use them only as
comparison points; obmm is free to keep its own NC FIFO + paired-desc layout
when chasing performance.

Conceptual flow on the **sender** side:

1. Reserve a slot in the peer's receive FIFO (atomic head increment).
2. Pack metadata into the FIFO element header.
3. For `am_short`, write `[header | payload]` inline after the FIFO element
   header; for `am_bcopy`, write the packed payload into the paired desc area
   for the same ring index.
4. Publish the slot with a release-style bus-domain fence followed by the
   owner/flags byte.
5. Return `UCS_OK` (or `UCS_ERR_NO_RESOURCE` if FIFO is full — UCP will
   retry via pending queue).

Conceptual flow on the **receiver** side, inside `iface_progress`:

1. Read local FIFO tail.
2. If a new slot is published (owner/flags byte matches), issue the matching
   bus-domain acquire fence.
3. Validate slot generation to drop stale writes after slot reuse.
4. Dispatch via `uct_iface_invoke_am(...)` using either the inline short
   buffer or the paired desc buffer.
5. Advance tail with the required bus-domain ordering.

For obmm, the FIFO and slot memory live in the **pre-imported peer memory
region** (see `obmm-api-and-env`), accessed via mmap'd virtual addresses, not
via `obmm_export/import` calls at runtime.

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

1. When you need exact macro signatures, hook wiring, or prior-art behavior,
   query the vector DB (see `vector-db-retrieval` skill) and inspect the exact
   UCX source directly. Do not guess macro signatures.
2. If you do use a reference file, cross-check it with a direct `view` to
   confirm the exact prototype, since UCX revisions may have shifted
   signatures.
3. After implementation, re-read `iface_query` and the ops tables to make
   sure capability bits and function pointers stay in sync.
