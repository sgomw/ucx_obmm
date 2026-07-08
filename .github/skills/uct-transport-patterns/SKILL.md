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
                                              `uct_tl_register`
- `ucx/src/uct/api/uct.h`                  — public ep / iface signatures

Current obmm design choices are documented in
`ucx/src/uct/obmm/DESIGN.md`; this skill only records UCX UCT framework
patterns that are expected to remain stable across obmm design iterations.

## Framework contracts that must NOT be broken

When editing UCT transport code, every one of these must remain consistent or
the transport will silently fail to load / register:

1. Component registration
   - The transport must define a `uct_component_t` and register its TLS entry
     with `UCT_TL_DEFINE_ENTRY(...)`.
   - Component init must register the component/TLS; cleanup must unregister
     TLS entries before unregistering the component.

2. Class hierarchy (UCS_CLASS_*)
   - Transport iface classes normally derive from `uct_base_iface_t`.
   - Transport ep classes normally derive from `uct_base_ep_t`.
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
   - When adding or removing a capability, update both the operation table and
     the corresponding flags/numeric caps in `iface_query()`.

5. Reachability
   - `iface_is_reachable_v2` is what UCP uses; the legacy
     `iface_is_reachable` field stays as `uct_base_iface_is_reachable`.
   - Reachability should match the transport's advertised address semantics
     from `DESIGN.md`; do not assume an `sm/` same-host check is sufficient for
     every UCT transport.

## AM Data-Path References

Reference candidates include `uct_mm_ep_am_short`, `uct_mm_ep_am_bcopy`,
`uct_tcp_ep_am_bcopy`, and the corresponding progress / capability code in the
relevant transports. Use them to confirm UCX callback signatures, operation
semantics, pending behavior, and capability exposure. Transport-specific FIFO
layout, shared-memory ownership, fences, and wire format belong in
`DESIGN.md`.

## Helper macros worth knowing

- `UCT_CHECK_AM_ID(am_id)`  — validate AM id at entry of AM send ops
- `UCT_CHECK_LENGTH(length, 0, max_short, "am_short")`
- `UCT_TL_EP_STAT_OP(ep, AM, SHORT, length)`
- `uct_iface_invoke_am(iface, id, data, len, flags)`
- `ucs_derived_of(p, type)`  — downcast
- `UCS_CLASS_CALL_SUPER_INIT(super_t, ...)`
- `UCS_STATIC_BITMAP_*`, `ucs_arbiter_*`

## Required workflow when touching transport code

1. When external UCX framework context is needed, query the vector DB (see
   `vector-db-retrieval` skill) for the closest relevant analogue. Do not
   guess macro signatures or assume the answer must come from `sm/`.
2. Cross-check the chosen reference file with a direct `view` to confirm
   the exact prototype, since UCX revisions may have shifted signatures.
3. After implementation, re-read `iface_query` and the ops tables to make
   sure capability bits and function pointers stay in sync.
