# Copilot Instructions

## Build, test, and verification

- **Build MPI tests** (Linux build host): `./build_mpi_tests.sh`
- **Run MPI transport suite** (real two-node setup): `./run_mpi_tests.sh node0 node1`
- **Run OSU micro-benchmarks** (real two-node setup): `OSU_DIR=/path/to/osu ./run_osu_tests.sh node0 node1`

This Windows workspace has no OBMM hardware and no Linux toolchain.  Local
verification is limited to static review.  Real transport validation happens
on the Linux two-node setup.

## High-level architecture

This is a three-repo integration tree:

- `ucx/` — active transport development.  `ucx/src/uct/obmm/base/` is the
  **legacy NC transport** (validated, but NC is being deprecated).  The CC
  redesign will also live under `ucx/src/uct/obmm/`.
- `obmm/` — libobmm (read-only): API, consistency model, device documentation.
- `ompi/` — MPI (read-only): shows how UCP/UCT are selected and driven.

## NC → CC transition

NC memory is being removed.  The current transport (NC FIFO + SPSC short lanes)
cannot be adapted — it assumes concurrent cross-host read/write, which OBMM's
cacheable consistency model forbids.  **A from-scratch CC-only redesign is
required.**

The OBMM cacheable consistency model is the single most important constraint:

> At any moment, all hosts touching a region must be either all PROT_READ /
> PROT_NONE, or exactly one PROT_WRITE with all others PROT_NONE.

## Key conventions

- Start with `AGENTS.md`. It is the repository workflow.
- For UCX/OBMM/OMPI code-grounded reasoning, query the vector DB via
  `.claude/skills/vector-db-retrieval/query_chroma.py`, then read exact files.
- Before designing CC transport logic, re-read `.claude/skills/obmm-api-and-env`
  for the CC model, and `.claude/skills/uct-transport-patterns` for UCX contracts.
- `ompi/` and `obmm/` are read-only unless explicitly asked.
- CC transport edits belong under `ucx/src/uct/obmm/` and `ucx/src/uct/Makefile.am`.
- Keep these surfaces in sync: ops table, internal ops table, `iface_query()`,
  address structs, reachability, and the design document.
- Do not copy the legacy NC FIFO/pool/fence/SPSC-lane patterns.
- Do not call `obmm_export/unexport/import/unimport/preimport/unpreimport` from UCT.
- Key peers by `(exporter_dcna, exporter_deid, memid)`, never memid alone.
- Ask before inventing libobmm semantics, hardware behavior, or protocol
  assumptions that cannot be verified locally.
