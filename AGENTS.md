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
- Current NC baseline is the **receiver-local sharded atomic FIFO** design:
  `am_short`, `am_bcopy`, per-shard head CAS with explicit arm64 LSE,
  pending dispatch, strict memid/discovery handling, zero-on-exit cleanup,
  and tuned pool geometry. Preserve existing behavior unless the task is
  explicitly to change it.
- Current hardware fact for NC atomics: cross-node 64-bit FAA/CAS are
  available only when emitted as explicit arm64 **LSE** atomics; compiler-
  default **LL/SC** sequences are not supported on the target NC mapping.
- PUT/GET/RMA/zcopy/atomics are not implemented and must remain unsupported
  unless a separate design is approved.
- For geometry tuning, prefer **64-byte-aligned** `FIFO_ELEM_SIZE` and
  `BCOPY_SEG_SIZE` unless new measurements prove otherwise. Non-64B-aligned
  strides have regressed measured latency on the current platform.
- Current validated runtime baseline from user hardware runs: OSU
  point-to-point and collective test suites pass on the target setup.

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
   - hang: inspect progress, pending, shard head/tail/owner-bit publication,
     backpressure, and ownership state before changing fences or wire format
   - performance regression: trace the actual MPI → PML UCX → UCP path,
     confirm whether proto v2 is active, and check alignment/threshold effects
     before retuning raw UCT caps

4. **Plan and critique non-trivial changes.**
   Update the session `plan.md` for design-level changes. For ops table,
   capability, wire-format, ownership, reachability, or tuning changes, run a
   design critique (`rubber-duck` if available; otherwise a synchronous review
   agent or explicit self-review checklist) before implementation.

5. **Implement coherently.**
   Mirror mm/self structure for UCX framework wiring, but do not copy their
   memory capabilities blindly. Keep these in sync:
   - ops table entries
   - `iface_query` capability flags and numeric caps
   - iface/device address structs
   - reachability checks and diagnostics
  - receiver-local FIFO bank/shard mapping and local progress semantics,
    including one-way traffic that may arrive without reverse local `ep_create`
   - pool geometry / wire format / `DESIGN.md`

6. **Verify in the right environment.**
   Follow `ucx-build-verify`. In this Windows workspace, do not waste time
   trying to run Linux UCX build commands. If no Linux shell/toolchain is
   available, do static checks locally and hand the build commands to the user
   or a Linux build host. Do not claim a feature works if it cannot be
   observed by `ucx_info -d -t obmm`, `ucx_info -c`, symbol inspection, or
   user-provided benchmark data.

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
- Do not re-introduce a separate shared lock word or rely on compiler-default
  LL/SC atomics on arm64; the current FIFO data path uses explicit LSE CAS only
  for shard-head reservation.
- For any future NC atomic probe or experiment, do not rely on compiler-default
  atomics on arm64; use explicit **LSE** instruction sequences.
- Do not assume every receive edge has a local EP. One-way collective traffic
  exists, so receive progress must keep working even when the inbound lane was
  not registered by local `ep_create`.
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
