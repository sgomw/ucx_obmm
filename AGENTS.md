# Agent Operating Rules — ucx_obmm_br

This file is the mandatory workflow for agents working in this repository. The
project implements a UCX UCT transport (`obmm`) against hardware that is not
available in the local development environment, so the main risk is
hallucinating UCX framework behavior, OBMM runtime semantics, or hardware facts.

Before non-trivial work, read:

- `.github/skills/vector-db-retrieval/SKILL.md` — local retrieval workflow.
- `.github/skills/uct-transport-patterns/SKILL.md` — UCX UCT contracts.
- `.github/skills/obmm-api-and-env/SKILL.md` — OBMM API/topology facts.
- `.github/skills/ucx-build-verify/SKILL.md` — environment-gated verification.
- `ucx/src/uct/obmm/DESIGN.md` — current wire format and data-path design.

## Scope reminder

- Repos: `ucx/`, `obmm/`, `ompi/`.
- Active transport development is in `ucx/src/uct/obmm/`.
- `ompi/` is read-only context.
- `obmm/` is libobmm context; do not extend its API for transport work.
- V2 NC baseline is stable: `am_short`, `am_bcopy`, pending arbiter, and
  cross-node MPI/OSU bring-up fixes. Preserve `MEM_MODE=nc` behavior.
- V3 target is hybrid: NC FIFO/control/short plus CC chunks for selected
  `am_bcopy` payloads. Pure CC FIFO/control is not a target.
- PUT/GET/RMA/zcopy/atomics are not implemented and must remain unsupported
  unless a separate design is approved.

## Mandatory workflow for obmm transport changes

1. **Retrieve before reasoning.**
   Query the local vector DB before making code-grounded UCX/OBMM/OMPI
   conclusions. Direct file reads are for confirming exact code after retrieval.

2. **Re-read environment facts.**
   Read `obmm-api-and-env` before touching memory setup, mmap, ownership,
   addressing, reachability, or hardware assumptions. If a fact is missing,
   ask the user; do not invent it.

3. **Diagnose before changing code.**
   For failures, first locate evidence:
   - Literal UCX/OMPI error string: grep the string and read the emit site.
   - Stack trace: read from the top UCX/UCP function down to the transport
     capability/ops that selected the path.
   - Hang: inspect progress, pending, FIFO backpressure, and ownership state
     before changing fences or wire format.

4. **Plan and review non-trivial changes.**
   Update the session `plan.md` for design-level changes. For ops table,
   capability, wire-format, ownership, or reachability changes, run a design
   critique (`rubber-duck` if available; otherwise a synchronous review agent
   or an explicit self-review checklist) before implementation.

5. **Implement coherently.**
   Mirror mm/self structure for UCX framework wiring, but do not copy their
   memory capabilities blindly. Keep these in sync:
   - ops table entries
   - `iface_query` capability flags and numeric caps
   - iface/device address structs
   - reachability checks and diagnostics
   - pool version / wire format / `DESIGN.md`

6. **Verify in the right environment.**
   Follow `ucx-build-verify`. In this Windows workspace, do not waste time
   trying to run Linux UCX build commands. If no Linux shell/toolchain is
   available, do static checks locally and hand the build commands to the user
   or a Linux build host.

7. **Code-review before declaring done.**
   Run a high-signal code review on the diff. Adopt findings that prevent
   correctness, compile, resource-lifetime, or protocol bugs; skip style-only
   feedback.

## Hard rules

- Do not call `obmm_export`, `obmm_unexport`, `obmm_import`, `obmm_unimport`,
  `obmm_preimport`, or `obmm_unpreimport` from inside the UCT transport.
- In NC mode, do not call `obmm_set_ownership`.
- In V3 hybrid, `obmm_set_ownership` is allowed only for CC mappings/chunks and
  only according to `DESIGN.md` and `obmm-api-and-env`.
- Do not run `mpirun`, `ucx_perftest`, OSU, or hardware/two-node tests locally.
- Do not modify `ompi/` or `obmm/` unless the user explicitly asks.
- Do not modify files outside `ucx/src/uct/obmm/`, `ucx/src/uct/Makefile.am`,
  and needed build wiring without user approval, except for explicit workflow,
  test, or documentation tasks requested by the user.
- Do not advertise a UCT/MD capability unless the corresponding operation and
  memory semantics are truly implemented in obmm.
- Do not use memid as a cross-node peer key. Import memid is local to the
  importing node; match peers by exporter identity.
- Do not bypass this workflow for capability bits, class macros, ops tables,
  wire format, reachability, or ownership logic. Those are the high-risk areas.

## Agent assignments

| Activity | Preferred approach |
| --- | --- |
| UCX/OBMM/OMPI code-grounded research | `vector-db-retrieval`, then direct file reads |
| Targeted symbol lookup | direct `rg`/`view` after retrieval |
| Design critique | rubber-duck/review agent or explicit self-review |
| Coupled md/iface/ep implementation | primary agent/self, not split across low-context workers |
| Verbose Linux build/test execution | `task` agent only when Linux toolchain exists |
| Final diff review | `code-review` |

## When to stop and ask

Ask the user before assuming:

- hardware behavior not documented in `obmm-api-and-env`
- NC/CC role, ownership granularity, mmap offset, or cache-coherence semantics
- changes to libobmm or OMPI
- broad scope changes such as PUT/GET/RMA/zcopy

The cost of one clarification is lower than baking an unverifiable hardware
assumption into a UCT transport.
