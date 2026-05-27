# Copilot Instructions

## Build, test, and verification

- **UCX developer build** (run from `ucx/`): `./autogen.sh && ./contrib/configure-devel --prefix=$PWD/install && make -j && make install`
- **UCX release build** (run from `ucx/`): `./autogen.sh && ./contrib/configure-release --prefix=/path/to/install && make -j && make install`
- **No-hardware obmm verification** (run from `ucx/` after install): `./install/bin/ucx_info -d -t obmm_cc`, `./install/bin/ucx_info -d -t obmm_nc`, `./install/bin/ucx_info -c | grep -i OBMM`, `nm -D ./install/lib/libuct.so | grep uct_obmm`
- **UCX internal unit tests** (run from `ucx/`): `make -C test/gtest test`
- **Build the MPI transport tests** (run from repo root on a Linux build host with the repo's OMPI in `PATH`): `./build_mpi_tests.sh`
- **Run the full MPI transport suite** (real two-node setup only, from repo root): `./run_mpi_tests.sh node0 node1`
- **Run a single MPI transport test**: `ONLY=pingpong ./run_mpi_tests.sh node0 node1`
- **Run only the v2 MPI suite**: `ONLY=v2 ./run_mpi_tests.sh node0 node1`
- **Run the micro-benchmark suite** (real two-node setup only): `OSU_DIR=/path/to/osu ./run_osu_tests.sh node0 node1`
- **Run one micro-benchmark category**: `CAT=pt2pt OSU_DIR=/path/to/osu ./run_osu_tests.sh node0 node1`
- **UCX static checks**: `ucx/buildlib/tools/static_checks.sh` exists, but it is CI-oriented and expects the UCX checker environment from `buildlib/tools/common.sh`

For obmm transport work, this Windows workspace is analysis-only: do not try to run `mpirun`, `ucx_perftest`, or any two-node hardware test locally. Local verification is limited to build success plus `ucx_info` and symbol inspection; real transport validation happens on the Linux two-node setup.

## High-level architecture

- This workspace is a three-repo integration tree: `ucx/` contains the active obmm UCT transport, `obmm/` provides libobmm API and sysfs/runtime semantics, and `ompi/` is read-only context for understanding how MPI traffic reaches UCP/UCT.
- The active transport code is in `ucx/src/uct/obmm/base/`. `obmm_md.c` discovers `/sys/devices/obmm/obmm_shmdev*`, maps the configured NC and CC regions, and intentionally does not advertise rkey-based access.
- `obmm_iface.c` owns transport capabilities, reachability, progress, and slot allocation. The current public TLs are `obmm_cc` for same-node peers and `obmm_nc` for cross-node peers; both reuse the same iface class with role-specific capability and reachability gates.
- `obmm_ep.c` is the send path. `obmm_cc` serves same-node short/eager traffic, while `obmm_nc` keeps the cross-node NC eager/control path; both still share the internal CC bulk-window machinery behind `am_bcopy`.
- `obmm_pool.c`, `obmm_region.c`, and `obmm_sysfs.c` implement the shared-memory substrate: two-phase pool initialization, crash-safe slot reuse with generation tracking, NC mmap of shmdev devices, and exporter/importer discovery from sysfs.
- `ucx/src/uct/obmm/DESIGN.md` is the authoritative transport design document. It defines the shared-region layout, FIFO/descriptor geometry, fence rules, capability scope, and wire-format compatibility; update it before changing transport layout, geometry, or advertised capabilities.
- The performance and selection path spans repositories: OMPI's PML UCX code in `ompi/ompi/mca/pml/ucx/` creates UCP endpoints, UCP selects lanes/protocols, and then the obmm UCT iface/ep implementation serves AM operations underneath.

## Key conventions

- Start with `AGENTS.md`. It is the repository workflow, not optional guidance.
- For UCX/OBMM/OMPI code-grounded reasoning, query the local vector DB first via `.github/skills/vector-db-retrieval/query_chroma.py`, then read the exact files you need.
- Before changing obmm transport logic, re-read the skill docs under `.github/skills/`: `obmm-api-and-env` for hardware/runtime facts, `uct-transport-patterns` for UCX framework contracts, and `ucx-build-verify` for what can actually be validated in this environment.
- `ompi/` is read-only context. `obmm/` is libobmm context and should not be extended for transport work unless explicitly requested. Normal obmm transport edits belong under `ucx/src/uct/obmm/` and, when wiring sources, `ucx/src/uct/Makefile.am`.
- Keep these surfaces in sync whenever capabilities change: the iface ops table, internal ops table, `iface_query()` capability flags and numeric caps, iface/device address structs, reachability checks, and `DESIGN.md`.
- The transport is active-message only. `obmm_cc` and `obmm_nc` both support `am_short` + `am_bcopy`; pending, connect-to-iface, and synchronous callbacks remain enabled, `INTER_NODE` is only advertised on `obmm_nc`, and PUT/GET/RMA/zcopy/atomics stay unsupported unless a separate design changes that.
- Do not call `obmm_export`, `obmm_unexport`, `obmm_import`, `obmm_unimport`, `obmm_preimport`, or `obmm_unpreimport` inside the UCT transport. `obmm_set_ownership` is allowed only on the isolated CC bulk-window path, not on the NC eager path.
- Treat exporter identity as `(exporter_dcna, exporter_deid, memid)`; never key peers by memid alone.
- On arm64 NC mappings, obmm shared control words must use explicit LSE atomics; do not rely on generic compiler-lowered `ucs_atomic_*` or LL/SC atomics.
- Use bus-domain fences on the obmm data path, not the CPU-domain fences used by mm.
- Preserve the measured geometry defaults unless the task is explicitly to retune them: `obmm_nc` keeps NC eager/control on `FIFO_SIZE=64`, `FIFO_ELEM_SIZE=64`, `BCOPY_SEG_SIZE=32768`, `BW=3400MBs`; current phase-1 `obmm_cc` work is reverting to a b4-style same-node CC eager ring with direct `desc[N]` packing rather than the later receiver-owned desc-pool design. Keep FIFO and bcopy strides 64-byte aligned unless new measurements justify changing them.
- For hard transport bugs, follow the repo's debug workflow: minimize the reproducer first, add logs to locate the failure clearly, and only then change transport logic.
