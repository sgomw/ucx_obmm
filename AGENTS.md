# Agent Operating Rules — ucx_obmm_br

This file is the **mandatory workflow** for any agent (human or AI) working
in this repository. The project implements a UCX UCT transport (`obmm`)
against hardware that is not available in the local development environment,
so the main risks are hallucinating UCX framework behavior, OBMM runtime
semantics, hardware facts, and protocol-selection behavior above UCT.

Before non-trivial work, read:

- `.github/skills/vector-db-retrieval/SKILL.md` — local retrieval workflow.
- `.github/skills/uct-transport-patterns/SKILL.md` — UCX UCT contracts.
- `.github/skills/obmm-api-and-env/SKILL.md` — OBMM API/topology facts.
- `.github/skills/ucx-build-verify/SKILL.md` — environment-gated verification.
- `ucx/src/uct/obmm/DESIGN.md` — current wire format, pool layout, and tuning.

## Scope reminder

- Repos: `ucx/`, `obmm/`, `ompi/`.
- Active transport development is in `ucx/src/uct/obmm/`.
- `ompi/` is **read-only context**.
- `obmm/` is libobmm context; do not extend its API for transport work.
- Current NC transport is AM-only and includes FIFO-backed `am_short`,
  paired-desc FIFO `am_bcopy`, pending dispatch,
  strict exporter-identity/discovery handling, zero-on-exit cleanup, and
  tuned pool geometry. The current default geometry uses one NC region per node
  with at least 2,441,099,584 bytes for 96 local iface/process slots and a
  192 KiB NC bcopy segment. Dedicated SPSC short lanes have been removed;
  `short_lane_count` is kept on the wire as 0 to reject stale lane-based peers.
  The transport does not expose private cleanup-time performance/statistics log
  knobs. The earlier AM-only baseline passed the full OSU micro-benchmark suite
  on the real two-node setup; FIFO-only short routing and the 192 KiB NC bcopy
  geometry still require fresh target validation.
- PUT/GET/RMA/zcopy/atomics are not implemented and must remain unsupported
  unless a separate design is approved.
- For geometry tuning, prefer **64-byte-aligned** `FIFO_ELEM_SIZE` and
  `BCOPY_SEG_SIZE` unless new measurements prove otherwise. Non-64B-aligned
  strides have regressed measured latency on the current platform.
- On arm64 NC mappings, any obmm shared-memory atomic RMW must use explicit
  LSE instructions. Do not rely on compiler-default LL/SC emitted by generic
  atomic builtins or `ucs_atomic_*`.

## Mandatory workflow for obmm transport changes

1. **Retrieve before reasoning.**
   Query the local vector DB before making code-grounded UCX/OBMM/OMPI
   conclusions. Direct file reads are for confirming exact code after retrieval.

2. **Re-read environment facts.**
   Read `obmm-api-and-env` before touching memory setup, mmap, ownership,
   addressing, reachability, or hardware assumptions. If a fact is missing,
   ask the user; do not invent it.

3. **Diagnose before changing code.**
   For failures or regressions, first locate evidence:
   - literal UCX/OMPI error string: grep the string and read the emit site
   - stack trace: read from the top UCX/UCP function down to the transport
     capability/ops that selected the path
   - hang: inspect progress, pending, FIFO backpressure, and ownership state
     before changing fences or wire format
   - performance regression: trace the actual MPI → PML UCX → UCP path,
     confirm whether proto v2 is active, and check alignment/threshold effects
     before retuning raw UCT caps

4. **Plan and critique non-trivial changes.**
   Update the session `plan.md` for design-level changes. For ops table,
   capability, wire-format, ownership, reachability, or tuning changes, run a
   design critique (`rubber-duck` if available; otherwise a synchronous review
   agent or explicit self-review checklist) before implementation.

5. **Implement coherently.**
   Follow UCX framework contracts and choose the most relevant in-tree
   transport patterns for the capability you are touching; do not treat
   `sm/` transports as mandatory references, and do not copy any transport's
   memory capabilities blindly. Keep these in sync:
   - ops table entries
   - `iface_query` capability flags and numeric caps
   - iface/device address structs
   - reachability checks and diagnostics
   - pool geometry / wire format / `DESIGN.md`

6. **Verify in the right environment.**
   Follow `ucx-build-verify`. In this Windows workspace, do not waste time
   trying to run Linux UCX build commands. If no Linux shell/toolchain is
   available, do static checks locally and hand the build commands to the user
   or a Linux build host. Do not claim a new behavior works locally if it
  cannot be observed by `ucx_info -d -t obmm`, `ucx_info -c`, symbol
  inspection, or user-provided benchmark data. An earlier AM-only baseline
  passed the full OSU suite on the real two-node setup; use that as the prior
  reference point when reasoning about regressions.

7. **Code-review before declaring done.**
   Run a high-signal code review on the diff. Adopt findings that prevent
   correctness, compile, resource-lifetime, or protocol bugs; skip style-only
   feedback.

## Hard rules

- Do not call `obmm_export`, `obmm_unexport`, `obmm_import`, `obmm_unimport`,
  `obmm_preimport`, or `obmm_unpreimport` from inside the UCT transport.
- In the current NC transport, do not call `obmm_set_ownership`.
- Do not run `mpirun`, `ucx_perftest`, or any hardware/two-node test locally.
- Do not modify `ompi/` or `obmm/` unless the user explicitly asks.
- Do not modify files outside `ucx/src/uct/obmm/`,
  `ucx/src/uct/Makefile.am`, and needed build wiring without user approval,
  except for explicit workflow or documentation tasks requested by the user.
- Do not advertise a UCT/MD capability unless the corresponding operation and
  memory semantics are truly implemented in obmm.
- Do not use memid as a cross-node peer key. Match peers by exporter identity.
- Do not use generic compiler-lowered atomics on arm64 NC mappings; obmm
  shared control words must use explicit LSE atomics.
- Do not invent libobmm semantics, device paths, mmap offsets, cache
  coherence, or protocol behavior above UCT. Ask the user when facts are
  missing.
- Do not bypass this workflow for capability bits, class macros, ops tables,
  wire format, reachability, ownership logic, or geometry tuning.

## Agent assignments

| Activity | Preferred approach |
| --- | --- |
| UCX/OBMM/OMPI code-grounded research | `vector-db-retrieval`, then direct file reads |
| Targeted symbol lookup | direct `rg`/`view` after retrieval |
| Design critique | `rubber-duck`/review agent or explicit self-review |
| Coupled md/iface/ep implementation | primary agent/self, not split across low-context workers |
| Verbose Linux build/test execution | `task` agent only when Linux toolchain exists |
| Final diff review | `code-review` |

## When to stop and ask

Ask the user before assuming:

- hardware behavior not documented in `obmm-api-and-env`
- NC memory semantics or any future CC/ownership semantics
- changes to libobmm or OMPI
- broad scope changes such as PUT/GET/RMA/zcopy
- benchmark conclusions that need more data than the current measurements cover

The cost of one clarification is lower than baking an unverifiable assumption
into a UCT transport.
