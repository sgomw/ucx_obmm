---
name: uct-transport-patterns
description: >
  UCX UCT transport-layer development patterns. Use when implementing or
  modifying any UCT transport (md / iface / ep), especially obmm.
---

# UCT Transport Patterns

UCX UCT is macro- and hook-table-driven. obmm code must mirror framework
structure from existing transports while preserving OBMM-specific memory
semantics.

## Reference transports

Read these before changing obmm framework wiring:

- `ucx/src/uct/sm/mm/base/mm_md.{c,h}` — shared-memory MD base
- `ucx/src/uct/sm/mm/base/mm_iface.{c,h}` — FIFO, progress, pending dispatch
- `ucx/src/uct/sm/mm/base/mm_ep.{c,h}` — AM short/bcopy and pending pattern
- `ucx/src/uct/sm/mm/posix/mm_posix.c` — concrete mm provider
- `ucx/src/uct/sm/mm/sysv/mm_sysv.c` — concrete mm provider
- `ucx/src/uct/sm/self/` — minimal single-process transport
- `ucx/src/uct/sm/base/sm_iface.{c,h}` — `uct_sm_iface_t` superclass
- `ucx/src/uct/base/uct_iface.h` — ops tables and class/entry macros
- `ucx/src/uct/api/uct.h` — public UCT signatures

Use retrieval first, then direct reads for exact signatures.

## Framework contracts

Keep these surfaces synchronized:

1. Component registration
   - `UCT_TL_DEFINE_ENTRY(&uct_obmm_component, obmm, ...)`
   - `UCT_SINGLE_TL_INIT(&uct_obmm_component, obmm, ...)`
   - `uct_component_t uct_obmm_component`

2. Class hierarchy
   - `uct_obmm_iface_t` derives from `uct_sm_iface_t`
   - `uct_obmm_ep_t` derives from `uct_base_ep_t`
   - INIT/CLEANUP/DEFINE/NEW/DELETE macros must match.

3. Ops tables
   - Every `uct_iface_ops_t`, `uct_iface_internal_ops_t`, and `uct_md_ops_t`
     field must be set.
   - Unsupported fields use `ucs_empty_function_return_unsupported` cast to
     the correct function type.
   - Adding a capability requires both a real ops entry and matching
     `iface_query` caps/numeric limits.

4. Capability bits
   - UCP selects protocols from `cap.flags` and numeric caps.
   - Do not advertise capabilities copied from mm unless obmm really implements
     the memory semantics.
   - `INTER_NODE` is required for cross-host OMPI/UCP address packing.

5. Reachability
   - UCP uses `iface_is_reachable_v2`; legacy `iface_is_reachable` can stay as
     `uct_base_iface_is_reachable`.
   - obmm reachability is based on exporter identity and configured mode/
     geometry, not same-host checks and not remote memid.

## obmm implemented AM patterns

### AM short

Reference: `uct_mm_ep_am_short` and `uct_mm_iface_progress`.

obmm differences:

- FIFO/control memory is NC.
- Publish/consume ordering uses bus-domain fences.
- Slot reservation uses load + CAS, not FAA.
- `max_short = fifo_elem_size - sizeof(uct_obmm_fifo_element_t)`.

### AM bcopy, NC mode

V2 NC bcopy uses a paired descriptor area inside each FIFO slot:

- FIFO element `N` owns desc `N`.
- Sender packs into `desc[N]`, fills elem metadata, bus-store fences, then
  publishes `OWNER | BCOPY`.
- Receiver invokes AM with `desc[N]`, flags `0`, and advances tail only after a
  full bus fence.
- `max_bcopy = BCOPY_SEG_SIZE`.

### AM bcopy, V3 hybrid mode

Hybrid keeps NC FIFO/control and uses CC chunks only for payload:

- Sender owns a local CC chunk with `PROT_WRITE`, packs into it, releases to
  `PROT_NONE`, then publishes an NC FIFO descriptor containing length, absolute
  chunk index, and CC exporter index.
- Receiver validates descriptor metadata, acquires `PROT_READ`, invokes AM with
  flags `0`, releases to `PROT_NONE`, then advances the NC tail.
- Sender reclaims chunk ownership after peer tail acknowledges the FIFO entry.
- `max_bcopy = CC_CHUNK_SIZE`.

## Pending pattern

If `UCT_IFACE_FLAG_PENDING` is advertised, implement real queueing:

- `pending_add` may return `UCS_ERR_BUSY` only when resources are already
  available and the caller should retry immediately.
- Otherwise push the pending request into an arbiter group and return `UCS_OK`.
- `iface_progress` must dispatch the arbiter and count send progress in its
  return value.
- Cleanup must purge pending requests before destroying ep/iface resources.

This pattern was required to fix symmetric OSU bibw pressure.

## Capability traps

- Do not advertise `RKEY_PTR`, `REG`, or `NEED_RKEY` unless obmm can map the
  peer's arbitrary user memory. Current obmm only maps OBMM shared regions.
- Do not advertise PUT/GET/RMA/zcopy/atomics until designed and implemented.
- Do not advertise EP_CHECK without a real cross-node liveness check.
- Do not set `UCT_CB_PARAM_FLAG_DESC` for obmm bcopy payloads; receive buffers
  are callback-ephemeral.

## Required self-check before finishing a transport change

1. Re-read `iface_query`.
2. Re-read ops tables.
3. Re-read iface/device address packing and reachability.
4. Re-read pool version / wire-format changes.
5. Confirm `DESIGN.md` matches the implementation.
6. Run code review on the diff.
