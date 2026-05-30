# Agent Operating Rules — ucx_obmm_br

This file is the **mandatory workflow** for any agent working in this
repository.  The project is transitioning from an NC (non-cacheable)
UCT transport to a **CC-only (cacheable) redesign** because the next
OBMM hardware revision drops NC support.

**The current `ucx/src/uct/obmm/` codebase is the legacy NC transport —
it works, it passed OSU, but it is NOT a blueprint for the CC redesign.**
The CC transport must be designed from scratch against the OBMM
cacheable consistency model.

Before non-trivial work, read:

- `.claude/skills/obmm-api-and-env/SKILL.md`     — OBMM CC model, topology, API
- `.claude/skills/uct-transport-patterns/SKILL.md` — UCX UCT framework contracts
- `.claude/skills/ucx-build-verify/SKILL.md`       — build & verify without hardware
- `.claude/skills/vector-db-retrieval/SKILL.md`    — local Chroma DB retrieval
- `ucx/src/uct/obmm/DESIGN.md`                     — **LEGACY** NC wire format (reference only)

## Scope reminder

- **Repos**: `ucx/` (transport dev), `obmm/` (libobmm, read-only), `ompi/` (MPI, read-only).
- **Active code**: future CC transport goes in `ucx/src/uct/obmm/`.
- **Legacy NC baseline**: the current `ucx/src/uct/obmm/base/` source tree plus
  `DESIGN.md`.  It advertises `AM_SHORT | AM_BCOPY | PENDING | CONNECT_TO_IFACE |
  CB_SYNC | INTER_NODE` via an NC FIFO + SPSC-short-lane design with bus-domain
  fences and LSE atomics.  It passed the full OSU micro-benchmark suite.
  **Treat as validated legacy, not as a design template for CC.**
- PUT / GET / RMA / zcopy / atomics remain unsupported unless a new design
  explicitly adds them.

## Design target: CC-only transport

NC memory is being removed from OBMM.  The next transport revision must run on
**cacheable mappings only**, respecting the OBMM cacheable consistency model:

> At any moment, all hosts touching a region must be in one of two states:
>   a) all hosts are PROT_READ or PROT_NONE, OR
>   b) exactly one host has PROT_WRITE; all other hosts are PROT_NONE.

The legacy NC FIFO pattern (concurrent cross-host read/write) is illegal under
this model.  The CC transport design must start from the consistency constraint
and work forward — not from the NC codebase.

## Mandatory workflow

1. **Understand the CC model first.**
   Read `obmm-api-and-env` before any design work.  The OBMM cacheable
   consistency model is the single most important constraint.

2. **Retrieve before reasoning.**
   Query the local vector DB before making UCX/OBMM/OMPI code-grounded
   conclusions.  Direct file reads are for confirming exact code after retrieval.

3. **Diagnose before changing code.**
   For failures or regressions, first locate evidence.  Do not guess.

4. **Plan and critique.**
   For design-level changes, write a short plan (the session `plan.md`).
   For ops table, capability, wire-format, or reachability changes, run a
   design critique before implementing.

5. **Implement coherently.**
   Follow UCX framework contracts.  Keep these in sync:
   - ops table entries
   - `iface_query` capability flags and numeric caps
   - iface / device address structs
   - reachability checks
   - `DESIGN.md` (or its CC replacement)

6. **Verify in the right environment.**
   This is a Windows workspace with no OBMM hardware and no Linux toolchain.
   Local verification is limited to static review.  Real validation requires
   the Linux two-node setup with MPI + OSU.
   Do NOT claim a new behavior works locally.

7. **Code-review before declaring done.**
   Run a high-signal review on the diff.  Adopt findings that prevent
   correctness, compile, resource-lifetime, or protocol bugs.

## Hard rules

- Do **not** call `obmm_export/unexport/import/unimport/preimport/unpreimport`
  from inside the UCT transport (lifecycle is external).
- Do **not** run `mpirun`, `ucx_perftest`, or any two-node test locally.
- Do **not** modify `ompi/` or `obmm/` unless explicitly asked.
- Do **not** modify files outside `ucx/src/uct/obmm/`, `ucx/src/uct/Makefile.am`,
  and build wiring without user approval.
- Do **not** advertise a UCT/MD capability unless it is truly implemented.
- Key peers by `(exporter_dcna, exporter_deid, memid)`, never memid alone.
- Do **not** invent libobmm semantics, device paths, mmap offsets, or hardware
  behavior.  Ask when facts are missing.
- Do **not** copy the legacy NC transport's FIFO, fence, or pool design
  patterns — they are invalid for CC.

## When to stop and ask

Ask the user before assuming:

- hardware behavior not documented in `obmm-api-and-env`
- changes to libobmm or OMPI
- new capability surfaces (PUT/GET/RMA/zcopy)
- benchmark conclusions that need more data than the current measurements cover
- that the legacy NC code can be incrementally patched rather than redesigned

The cost of one clarification is lower than baking an unverifiable assumption
into a UCT transport — especially one that must work on hardware you cannot
test locally.
