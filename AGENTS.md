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
- Current obmm transport design, capability surface, lifecycle, wire format,
  geometry, and rejected directions are documented in
  `ucx/src/uct/obmm/DESIGN.md`. Do not duplicate those design details in
  skills or workflow files; update `DESIGN.md` when the design changes.
- Environment/API facts that should remain stable across design iterations are
  documented in `.github/skills/obmm-api-and-env/SKILL.md`.

## Command execution and sandboxing

- If a command needed for diagnosis, inspection, or verification is blocked by
  sandboxing, first retry it with sandbox escalation/approval instead of
  dropping the check. Ordinary read-only commands are generally expected to be
  approved unless there is a specific safety concern.
- Do not use escalation to bypass this workflow's hard rules. Destructive
  commands, hardware/two-node tests, and changes outside the approved scope
  still require explicit user approval.

## Mandatory workflow for obmm transport changes

1. **Retrieve when it adds signal.**
   Query the local vector DB before making code-grounded conclusions about
   UCX framework behavior, libobmm behavior, or OMPI/MPI paths outside the
   active obmm transport. For current files under `ucx/src/uct/obmm/`, direct
   `rg`/file reads are sufficient and usually preferable because the retrieval
   collection intentionally excludes that directory.

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
   For any change that affects obmm lifecycle, addressing, capabilities, wire
   format, ownership, mapping policy, geometry, or protocol-visible behavior,
   update `ucx/src/uct/obmm/DESIGN.md` first and only then edit code.

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

- Do not run `mpirun`, `ucx_perftest`, or any hardware/two-node test locally.
- Do not modify `ompi/` or `obmm/` unless the user explicitly asks.
- Do not modify files outside `ucx/src/uct/obmm/`,
  `ucx/src/uct/Makefile.am`, and needed build wiring without user approval,
  except for explicit workflow or documentation tasks requested by the user.
- Do not invent libobmm semantics, device paths, mmap offsets, cache
  coherence, or protocol behavior above UCT. Ask the user when facts are
  missing.
- Do not bypass `DESIGN.md` for capability bits, class macros, ops tables,
  wire format, reachability, ownership logic, or geometry tuning.
- Do not implement an obmm design-affecting code change before the intended
  behavior is captured in `ucx/src/uct/obmm/DESIGN.md`.

## Agent assignments

| Activity | Preferred approach |
| --- | --- |
| External UCX/OBMM/OMPI code-grounded research | `vector-db-retrieval`, then direct file reads |
| Active obmm symbol lookup | direct `rg`/`view`; retrieval only when external context is needed |
| Design critique | `rubber-duck`/review agent or explicit self-review |
| Coupled md/iface/ep implementation | primary agent/self, not split across low-context workers |
| Verbose Linux build/test execution | `task` agent only when Linux toolchain exists |
| Final diff review | `code-review` |

## When to stop and ask

Ask the user before assuming:

- hardware behavior not documented in `obmm-api-and-env`
- NC memory semantics or any future cacheable ownership semantics
- changes to libobmm or OMPI
- broad scope changes not already covered by `DESIGN.md`
- benchmark conclusions that need more data than the current measurements cover

The cost of one clarification is lower than baking an unverifiable assumption
into a UCT transport.
