---
name: uct-transport-patterns
description: >
  UCX UCT transport-layer development patterns.  Use when implementing or
  modifying any UCT transport (md / iface / ep).  Keep UCX framework
  contracts but do NOT copy the legacy NC obmm data path — NC is deprecated.
---

# UCT Transport Patterns

UCX UCT is a heavily macro- and hook-table-driven framework.  Transport work
must follow UCX framework contracts; study the most relevant in-tree examples
for the capability being touched rather than inventing structure.

## ⚠️  Legacy NC obmm data path — do NOT copy

The current `ucx/src/uct/obmm/base/` transport is an **NC FIFO design** that
uses bus-domain fences, LSE atomics, and SPSC short lanes over `O_SYNC` mappings.
This pattern is **invalid for the CC redesign** because it assumes concurrent
cross-host read/write access.  Do not use it as a reference for the CC transport.

The UCX framework wiring (component registration, class macros, ops tables,
iface_query, reachability) in the legacy code IS still relevant — just not the
data path, fence, pool, or FIFO logic.

## Reference transports (in this repo)

- `ucx/src/uct/sm/mm/base/mm_{md,iface,ep}.{c,h}` — FIFO-style AM and pending wiring
- `ucx/src/uct/sm/self/` — minimal AM / endpoint wiring
- `ucx/src/uct/tcp/` — inter-node AM bcopy/zcopy (ownership model closer to CC than mm)
- `ucx/src/uct/ib/rc/base/`, `ucx/src/uct/ib/ud/base/` — capability exposure, RMA, perf modeling
- `ucx/src/uct/base/uct_iface.h` — `uct_iface_ops_t`, `UCT_TL_DEFINE_ENTRY`, `UCT_SINGLE_TL_INIT`
- `ucx/src/uct/api/uct.h` — public ep / iface signatures

## Framework contracts that must NOT be broken

1. **Component registration**
   - `UCT_TL_DEFINE_ENTRY(&uct_obmm_component, obmm, query_tl_devices_fn, iface_t, "OBMM_", config_table, config_t)`
   - `UCT_SINGLE_TL_INIT(&uct_obmm_component, obmm, ...)`
   - `uct_component_t uct_obmm_component = { ... }`

2. **Class hierarchy** (UCS_CLASS_*)
   - `uct_obmm_iface_t` derives from `uct_base_iface_t`
   - `uct_obmm_ep_t` derives from `uct_base_ep_t`
   - INIT / CLEANUP / DEFINE / DEFINE_NEW_FUNC / DEFINE_DELETE_FUNC must all be present.

3. **Ops tables** — set every field; unused fields → `ucs_empty_function_return_unsupported`.
   - `uct_iface_ops_t`, `uct_iface_internal_ops_t`, `uct_md_ops_t`
   - When adding a capability, update BOTH the ops-table entry AND `iface_query`.

4. **iface_query capability bits** — UCP selects the transport based on `cap.flags`
   and numeric caps.  Current legacy baseline: `AM_SHORT | AM_BCOPY | PENDING |
   CONNECT_TO_IFACE | CB_SYNC | INTER_NODE`.  The CC transport may start with the
   same surface or a subset.

5. **Reachability** — `iface_is_reachable_v2` is what UCP uses.  Match exporter
   identity plus wire geometry against the MD's mapped export/import regions.
   Do not regress to same-host-only checks.

## CC transport design considerations

The CC transport must work within the OBMM cacheable consistency model (see
`obmm-api-and-env`).  This means:

- **No concurrent bidirectional shared memory.**  Legacy patterns like "sender
  writes receiver's FIFO, receiver polls its own FIFO" are illegal.
- **Every access is an ownership handover.**  The transport must explicitly
  acquire and release access permissions.
- **Progress is different.**  The receiver cannot continuously poll a shared
  FIFO — it must acquire read permission (via syscall), check for work,
  release, and repeat.  Polling cost is a first-order design concern.
- **Address exchange must carry whatever information the peer needs to locate
  the right pages** (page offsets, ring geometry).

## Helper macros worth knowing

- `UCT_CHECK_AM_ID(am_id)`, `UCT_CHECK_LENGTH(length, 0, max, "name")`
- `UCT_TL_EP_STAT_OP(ep, AM, SHORT, length)`
- `uct_iface_invoke_am(iface, id, data, len, flags)`
- `ucs_derived_of(p, type)`, `UCS_CLASS_CALL_SUPER_INIT(super_t, ...)`
- `ucs_arbiter_*` — pending queue wiring (may still be relevant for CC)

## Required workflow

1. Query the vector DB (`vector-db-retrieval` skill) for the closest UCX analogue.
2. Cross-check the reference file with a direct read to confirm exact prototypes.
3. After implementation, re-read `iface_query` and ops tables to ensure consistency.
