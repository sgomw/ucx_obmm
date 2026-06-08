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
- Current target transport is AM-only and dual-plane. It registers logical TLS
  `obmm_nc` for cross-node NC AM and `obmm_cc` for same-node cacheable CC AM
  under the same `obmm` component. Both planes use FIFO-backed `am_short`,
  shared-data FIFO `am_bcopy`, pending dispatch, strict
  exporter-identity/discovery handling, metadata-reset-on-exit cleanup,
  slot-zero-on-allocation, and short-first pool geometry. The current NC
  environment uses one 3 GiB NC region per node; the default 96-slot geometry
  uses `FIFO_SIZE=128`, `FIFO_ELEM_SIZE=131200`, and
  `BCOPY_SEG_SIZE=131072`, requiring 1,612,200,256 bytes (1537.514 MiB).
  Dedicated SPSC short lanes
  have been removed; `short_lane_count` is kept on the wire as 0 to reject stale
  lane-based peers. The active wire format is
  `UCT_OBMM_WIRE_FORMAT_OVERLAP_DATA64` with a plane field in the iface address. The
  TLS can also run standalone: `obmm_cc` is CC-only and same-node-only, while
  `obmm_nc` allows same-node NC loopback only when no local CC export is
  configured, keeping dual-plane local traffic on CC.
  transport does not expose private cleanup-time performance/statistics log
  knobs. The earlier AM-only baseline passed the full OSU micro-benchmark suite
  on the real two-node setup; dual-plane routing still requires fresh target
  validation.
- Cross-node cacheable CC as a transport data path has been explored and
  rejected as of 2026-06-05. Do not extend, tune, or newly advertise staged CC
  `AM_ZCOPY`, receiver-owned CC, sender-owned CC, CC batch/epoch, or
  `UCT_OBMM_WIRE_FORMAT_CCZCOPY` as the performance route. The rejection is
  documented in `ucx/src/uct/obmm/DESIGN.md` and `plan.md`: ownership
  transitions dominate, sender-owned and receiver-owned variants did not meet
  high-concurrency latency goals, UCP AM_ZCOPY semantics do not naturally
  provide the required batching, and batch/epoch probing needs too much memory
  and adds latency. Same-node cacheable CC direct AM is approved because it
  does not use ownership transitions and is only reachable for peers on the
  same local CC export.
- PUT/GET/RMA/atomics remain unsupported unless a separate design is approved.
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
   - user-run diagnostics: keep added logs short, grep-friendly, and tied to a
     small number of decision points. Tell the user exactly which one to three
     lines or fields are needed, because hardware logs may have to be read and
     typed manually. Do not require bulk log copies unless there is no narrower
     diagnostic path.

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
  cannot be observed by `ucx_info -d -t obmm_nc`,
  `ucx_info -d -t obmm_cc`, `ucx_info -c`, symbol
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
- Never call `obmm_set_ownership` on NC mappings. Cross-node cacheable CC is no
  longer an approved transport data path; do not add new UCT ownership logic
  without an explicit new design from the user.
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
- NC memory semantics or any future cacheable ownership semantics
- changes to libobmm or OMPI
- broad scope changes such as PUT/GET/RMA, atomics, zcopy, or any cross-node
  cacheable CC/ownership transport path
- benchmark conclusions that need more data than the current measurements cover

The cost of one clarification is lower than baking an unverifiable assumption
into a UCT transport.
