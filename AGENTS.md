# Agent Operating Rules — ucx_obmm_br

This file is the **mandatory workflow** for any agent (human or AI) working
in this repository. It exists because the work — implementing a new UCT
transport (`obmm`) on top of UCX, against hardware that is not present in
the development environment — is high-risk for hallucination, framework
mis-wiring, and silent capability mismatches.

Skills referenced below live under `.github/skills/`:

- `vector-db-retrieval`     — semantic search over ucx / obmm / ompi
- `uct-transport-patterns`  — UCX UCT framework contracts and references
- `obmm-api-and-env`        — libobmm API + immutable test-env facts
- `ucx-build-verify`        — build commands and no-hardware verification

## Scope reminder

- Repos: `ucx/`, `obmm/`, `ompi/`. Active development is in `ucx/src/uct/obmm/`.
- `ompi/` is **read-only context** — MPI is the consumer above UCP; we do not
  modify it.
- `obmm/` is the libobmm we link against; we do not extend its API in this
  task.
- The only UCT communication semantic to implement right now is **am_short**.
  All other ep ops stay at `ucs_empty_function_return_unsupported`.

## Mandatory workflow (in order)

For any non-trivial change to `ucx/src/uct/obmm/`:

1. **Retrieve, do not recall.**
   Before reasoning about UCX framework code, query the vector DB via the
   `vector-db-retrieval` skill. Cite the path that drove each non-trivial
   conclusion. Direct file reads are allowed only after, or to confirm,
   a retrieval hit.

2. **Re-read the env facts.**
   Skim `.github/skills/obmm-api-and-env/SKILL.md` before designing
   anything that touches memory layout, mmap, addressing, or
   reachability. If a needed fact is missing there, **ask the user**
   with the `ask_user` tool — do not invent it.

3. **Plan, then rubber-duck the plan.**
   Write a short plan (in `plan.md` in the session folder, not in the
   repo) describing the proposed change: which files, which ops-table
   entries, which capability bits, which references from
   `uct-transport-patterns` are being mirrored.
   Then call the `rubber-duck` agent (sync) with the plan + the relevant
   reference snippets. Address its findings before implementing.

4. **Implement.**
   Mirror the closest reference transport (mm, then self) rather than
   inventing structure. Keep ops table, `iface_query` caps, and class
   macros in sync — see `uct-transport-patterns` for the contract list.

5. **Build and statically verify.**
   Run the build via the `task` agent and walk through the no-hardware
   checklist in `ucx-build-verify`. Do not claim a feature works if it
   cannot be observed by `ucx_info -d -t obmm`, `ucx_info -c`, or
   symbol inspection.

6. **Code-review.**
   Before declaring done, invoke the `code-review` agent on the diff.
   Treat its findings as evidence; adopt anything that prevents bugs,
   skip purely stylistic comments.

## Hard rules

- **Do not call** `obmm_export / obmm_unexport / obmm_import /
  obmm_unimport / obmm_preimport / obmm_unpreimport` from inside the UCT
  transport. Those happen outside UCX in the production environment.
  See `obmm-api-and-env` for why.
- **Do not run** `mpirun`, `ucx_perftest`, or any test that requires
  obmm hardware or a second node. There is none in this workspace.
- **Do not modify** files outside `ucx/src/uct/obmm/`,
  `ucx/src/uct/Makefile.am`, and (if needed) `ucx/configure.ac` /
  a new `ucx/src/uct/obmm/configure.m4`, without first asking the user.
- **Do not edit** anything in `obmm/` or `ompi/` for this task.
- **Do not invent** libobmm semantics, device paths, mmap offsets, or
  cache-coherence guarantees. Ask the user.
- **Do not bypass** the mandatory workflow above to "save a step" on
  changes that touch ops tables, class macros, capability bits, or
  reachability — those are exactly the places where silent breakage
  happens.

## Recommended agent assignments

| Activity                                  | Agent                |
|-------------------------------------------|----------------------|
| Broad cross-repo investigation            | `explore`            |
| Targeted symbol/file lookup               | direct grep/glob/view + `vector-db-retrieval` |
| Plan critique before implementation       | `rubber-duck` (sync) |
| Building UCX / running ucx_info           | `task`               |
| Final diff review                         | `code-review`        |
| Multi-file implementation work            | self, or `general-purpose` if the work spans many files |

## When in doubt

Stop and ask the user with `ask_user`. The cost of a clarifying question
is far lower than the cost of a wrong assumption baked into a UCT
transport that cannot be exercised on real hardware here.
