# CLAUDE.md — ucx_obmm_br

Project memory for the **obmm UCT transport** development repository.
**NC memory is being deprecated; the active task is a CC-only redesign.**

## Project identity

- **Goal**: Design and implement a **CC-only** UCX UCT transport (`obmm`) for
  OBMM shared-memory hardware, targeting a two-node ARM64 cluster.
- **Repos**: `ucx/` (transport dev), `obmm/` (libobmm — read-only), `ompi/` (MPI — read-only).
- **Active code**: `ucx/src/uct/obmm/` — both the legacy NC transport and the
  future CC transport live here.
- **Branch**: `er` (main: `main`).

## Legacy NC baseline (validated, NOT a CC template)

The code currently in `ucx/src/uct/obmm/base/` plus `DESIGN.md` is a
fully-validated **NC (non-cacheable) transport** that:

- Advertises `AM_SHORT | AM_BCOPY | PENDING | CONNECT_TO_IFACE | CB_SYNC | INTER_NODE`
- Uses `O_RDWR | O_SYNC` + `MAP_SHARED` for NC mappings
- Implements SPSC short lanes (64 × 8-deep × 256B) for am_short, CAS-FIFO for am_bcopy
- Relies on bus-domain fences (`dmb oshst`/`dmb oshld`) and LSE atomics
- Uses a two-phase pool init with crash-recovery slot lifecycle
- Passed the full OSU micro-benchmark suite on real hardware

**This design is invalid for CC** because it assumes concurrent read/write
access to the same region, which violates the OBMM cacheable consistency model.
Do not use it as a design reference for the CC transport.

## CC design constraints (primary)

OBMM cacheable mappings enforce a strict consistency model:

> At any moment, all hosts touching a region must be in one of two states:
>   a) all hosts are PROT_READ or PROT_NONE, OR
>   b) exactly one host has PROT_WRITE; all other hosts are PROT_NONE.

The legacy NC FIFO pattern (concurrent cross-host read/write) is illegal under
this model.  The CC transport must be designed from the consistency constraint
forward, without preconceptions about the data path, ownership protocol, or
page layout.

## Environment constraints

- **Two nodes** (node 0, node 1), each with one pre-exported 3072 MiB CC region.
- **ARM64 production ISA**.
- **No hardware in dev environment** and no Linux toolchain.  Local verification
  is limited to static review and design analysis.
- **Do NOT run** `mpirun`, `ucx_perftest`, or any two-node test locally.
- **Real validation** happens on the Linux two-node setup.

## Source file map

| File | Role |
|------|------|
| `ucx/src/uct/obmm/DESIGN.md` | **LEGACY** NC wire format, pool layout, sync rules |
| `ucx/src/uct/obmm/base/obmm_*.{c,h}` | **LEGACY** NC transport implementation |
| `obmm/` | libobmm (read-only): API header, consistency model docs |
| `ompi/` | MPI (read-only): PML UCX, protocol selection context |

## Build wiring

- obmm sources listed in `ucx/src/uct/Makefile.am` (noinst_HEADERS + libuct_la_SOURCES)
- No separate `configure.m4` or `Makefile.am` under `ucx/src/uct/obmm/`
- Build occurs on the Linux two-node setup, not in the dev workspace
## Hard rules

- Do NOT call `obmm_export/unexport/import/unimport/preimport/unpreimport` from UCT.
- Do NOT run hardware tests locally.
- Do NOT modify `ompi/` or `obmm/` without explicit ask.
- Do NOT advertise a capability unless truly implemented.
- Do NOT invent libobmm semantics or hardware behavior. Ask when facts are missing.
- Do NOT copy the legacy NC design patterns for the CC transport.
- Key peers by `(exporter_dcna, exporter_deid, memid)`, never memid alone.

## Test artifacts (root directory)

- `mpi_correctness_v2.c`, `mpi_pingpong_v2.c`, `mpi_bw_v2.c`, `mpi_multi_v2.c` — MPI transport tests
- `build_mpi_tests.sh`, `run_mpi_tests.sh`, `run_osu_tests.sh` — build/run infrastructure
