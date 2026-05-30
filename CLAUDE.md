# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository overview

This is a three-repo integration tree for developing a UCX UCT transport (`obmm_cc`, `obmm_nc`) on top of libobmm shared-memory fabric hardware.

- **`ucx/`** — Active development target. The obmm UCT transport lives in `ucx/src/uct/obmm/`.
- **`obmm/`** — libobmm API and sysfs/runtime semantics. Read-only context; do not extend its API for transport work.
- **`ompi/`** — Open MPI with OMPI PML UCX integration. Read-only context for understanding how MPI traffic reaches UCP/UCT.

`AGENTS.md` is the **mandatory repository workflow** — read it before non-trivial work. The skill files under `.github/skills/` are authoritative references for specific domains.

## Build, test, and verification

All commands run from their respective subdirectory unless noted.

### UCX developer build (from `ucx/`)

```sh
./autogen.sh
./contrib/configure-devel --prefix=$PWD/install
make -j
make install
```

### UCX release build

```sh
./autogen.sh
./contrib/configure-release --prefix=/path/to/install
make -j
make install
```

### No-hardware obmm verification (from `ucx/` after install)

```sh
./install/bin/ucx_info -d | grep -iE "obmm|Component"     # component registered
./install/bin/ucx_info -d -t obmm_cc                       # same-node caps
./install/bin/ucx_info -d -t obmm_nc                       # cross-node caps
./install/bin/ucx_info -c | grep -i OBMM                   # config keys exposed
nm -D ./install/lib/libuct.so | grep uct_obmm              # symbols exported
```

### UCX internal unit tests (from `ucx/`)

```sh
make -C test/gtest test
```

### MPI transport tests (repo root, Linux build host with in-tree OMPI on PATH)

```sh
./build_mpi_tests.sh                                       # build the test binaries
./run_mpi_tests.sh node0 node1                             # full suite (real HW only)
ONLY=pingpong ./run_mpi_tests.sh node0 node1              # single test
ONLY=v2 ./run_mpi_tests.sh node0 node1                    # v2 suite only
```

### OSU micro-benchmarks (repo root, real two-node setup only)

```sh
OSU_DIR=/path/to/osu ./run_osu_tests.sh node0 node1
CAT=pt2pt OSU_DIR=/path/to/osu ./run_osu_tests.sh node0 node1
```

### Critical: no local hardware

This Windows workspace is **analysis-only**. Do not run `mpirun`, `ucx_perftest`, or any two-node test locally. Local verification is limited to build success plus `ucx_info` and symbol inspection. Real transport validation requires the Linux two-node setup.

## Architecture

### UCX stack (top to bottom)

```
MPI (ompi/ompi/mca/pml/ucx/)
  → UCP (ucx/src/ucp/)          — tag-matching, protocols, lane selection
    → UCT (ucx/src/uct/)        — low-level transports
      → UCS (ucx/src/ucs/)      — data structures, arch abstractions, sys utils
```

### obmm transport internal structure

All active transport code is in `ucx/src/uct/obmm/base/`:

| File | Role |
|---|---|
| `obmm_md.{c,h}` | Memory domain: discovers `/sys/devices/obmm/obmm_shmdev*`, maps NC/CC regions from configured memids. Does **not** advertise rkey-based access. |
| `obmm_iface.{c,h}` | Interface: capabilities, reachability, slot allocation, progress polling, bulk window reclaim. Both TLs share this class. |
| `obmm_ep.{c,h}` | Endpoint: `am_short` (SPSC), `am_bcopy` (FIFO + bulk), pending queue. |
| `obmm_pool.{c,h}` | Shared-memory pool: two-phase init (UNINIT→INITING→READY), generation-tracked slots, crash-safe reuse. |
| `obmm_region.{c,h}` | NC/CC region mmap and lifetime. |
| `obmm_sysfs.{c,h}` | Exporter/importer discovery from sysfs. |
| `obmm_ownership.{c,h}` | `obmm_set_ownership` wrappers for CC bulk windows only. |
| `obmm_bulk.h` | Bulk control descriptor layout and window management. |
| `obmm_fifo.h` | FIFO element layout, bus-fence helpers, descriptor pairing. |
| `DESIGN.md` | **Authoritative** wire format, pool layout, fence rules, capability scope. Update before changing layout or geometry. |

### Two public TLs

- **`obmm_cc`** — Same-node-only. CC-mapped eager traffic. Advertises `AM_SHORT`, `AM_BCOPY`, `PENDING`, `CONNECT_TO_IFACE`, `CB_SYNC`. Built-in geometry: `FIFO_SIZE=8`, `FIFO_ELEM_SIZE=64`, `BCOPY_SEG_SIZE=65600`. Reports `max_bcopy=57344` to UCP (intentionally smaller than physical segment to avoid protocol regression).
- **`obmm_nc`** — Cross-node-optimized, also accepts same-node peers for single-TL bootstrap. NC-mapped eager/control + CC bulk windows. Advertises same AM/pending flags plus `INTER_NODE`. NC geometry: `FIFO_SIZE=64`, `FIFO_ELEM_SIZE=64`, `BCOPY_SEG_SIZE=32768`, `WINDOW_COUNT=16`, `WINDOW_SIZE=2m`.

Both TLs are AM-only. No PUT/GET/RMA/zcopy/atomics/EP_CHECK.

### Mapping classes

- **NC** (`open("/dev/obmm_shmdev*", O_RDWR | O_SYNC)` + `MAP_SHARED`): Cross-node eager/control traffic. Non-cacheable — bypasses OBMM consistency model. Supports cross-node atomic RMW.
- **CC** (`open(..., O_RDWR)` + mmap): Same-node eager and bulk-window storage. Cacheable, but requires `obmm_set_ownership` flips for cross-node bulk windows.

### Data path summary

```
am_short:  deterministic SPSC lane → bus_store_fence → advance head
am_bcopy (NC eager):  reserve FIFO slot (LSE CAS) → pack desc → bus_store_fence → publish flags
am_bcopy (NC bulk):   pack CC window → ownership flip → publish NC bulk descriptor → bus_store_fence → publish req_seq
Receiver:  drain SPSC lanes → drain legacy FIFO → bus_load_fence → validate generation → invoke_am
```

## Mandatory workflow

1. **Retrieve before reasoning.** Query the local Chroma vector DB before making UCX/OBMM/OMPI code conclusions:
   ```sh
   python .github/skills/vector-db-retrieval/query_chroma.py "<query>" --collection ucx_code --k 30
   python .github/skills/vector-db-retrieval/query_chroma.py "<query>" --collection obmm_code --k 10
   python .github/skills/vector-db-retrieval/query_chroma.py "<query>" --collection ompi_code --k 20
   ```
2. **Re-read environment facts** from `.github/skills/obmm-api-and-env/SKILL.md` before touching memory setup, mmap, ownership, addressing, or reachability.
3. **Diagnose before changing code.** Grep error strings, trace the UCP→UCT path, inspect progress/pending/FIFO state before changing fences or wire format.
4. **Plan and critique** in `plan.md` for design-level changes.
5. **Keep in sync:** ops tables, `iface_query` caps/flags, address structs, reachability, `DESIGN.md`.
6. **Verify** via build + `ucx_info` + symbol inspection. Flag anything requiring real hardware.
7. **Code-review** the diff before declaring done.

## Hard rules

These override any conflicting defaults from other UCX transports:

- **Do not call** `obmm_export`, `obmm_unexport`, `obmm_import`, `obmm_unimport`, `obmm_preimport`, or `obmm_unpreimport` from inside UCT.
- **Do not add** `obmm_set_ownership` to the NC eager path. It is allowed only on the isolated CC bulk-window path.
- **Do not run** `mpirun`, `ucx_perftest`, or any hardware/two-node test locally.
- **Do not modify** `ompi/` or `obmm/` without explicit user request.
- **Do not advertise** PUT/GET/RMA/zcopy/atomics/EP_CHECK unless a separate design is approved.
- **Do not use** generic compiler-lowered atomics (`ucs_atomic_*`, `__sync*`, `__atomic*`) on arm64 NC mappings. Shared control words must use explicit LSE helpers.
- **Do not use** `ucs_memory_cpu_*_fence` on the obmm data path. Use `ucs_memory_bus_store_fence()` / `ucs_memory_bus_load_fence()` for cross-host visibility.
- **Do not key peers by memid alone.** Use `(exporter_dcna, exporter_deid, memid)`.
- **Do not change** the single-CC-region split logic (CC eager prefix + CC bulk arena) without user approval.
- **Do not bypass** AGENTS.md for capability bits, class macros, ops tables, wire format, reachability, ownership logic, or geometry tuning.

## Key conventions

### Transport coherence

When changing capabilities, keep these surfaces in sync:
- ops table entries (`uct_iface_ops_t`, `uct_iface_internal_ops_t`, `uct_md_ops_t`)
- `iface_query()` capability flags and numeric caps
- iface/device address structs (`uct_obmm_cc_iface_addr_t`, `uct_obmm_iface_addr_t`, `uct_obmm_device_addr_t`)
- reachability checks in `iface_is_reachable_v2`
- pool geometry / wire format / `DESIGN.md`

### Fence ordering

- **Store-release**: `ucs_memory_bus_store_fence()` (aarch64 `dmb oshst`, x86 `sfence`) before publishing flags/head/tail.
- **Load-acquire**: `ucs_memory_bus_load_fence()` (aarch64 `dmb oshld`, x86 `lfence`) after observing owner bit, before reading payload.
- **Full barrier at tail publish**: `uct_obmm_bus_full_fence()` (aarch64 `dmb osh`, x86 `mfence`) before storing `recv_ctl->tail` — ensures all prior desc loads are ordered before the tail store that permits sender reuse.

### Current geometry defaults (do not change without retuning task)

| Knob | obmm_cc | obmm_nc |
|---|---|---|
| FIFO_SIZE | 8 | 64 |
| FIFO_ELEM_SIZE | 64 | 64 |
| BCOPY_SEG_SIZE | 65600 | 32768 |
| WINDOW_SIZE | — | 2 MiB |
| WINDOW_COUNT | — | 16 |
| BW (UCP estimate) | — | 3400 MB/s |
| SLOT_COUNT (compile-time) | 70 | 70 |
| SHORT_LANE_COUNT | 140 | 140 |

### Concurrent debugging session (branch `fr`)

A 32K bulk-path stall investigation is in progress. See memory file `obmm-nc-32k-hang-debug` and the repo-root `ucx_obmm_br_context_2026-05-30.md` for full state. Key facts:
- `rxB ~= 2 * txB` suggests duplicate bulk descriptor delivery across multiple EPs sharing a peer bulk-control stream.
- RX-side `ack_generation` early-claim mitigation is applied but not yet hardware-validated.
- User log constraint: target environment has no copy-paste; diagnostic logs must be single-line, key-field oriented, one round only.
- Next step: user runs one 32K test with `UCX_OBMM_DEBUG_LOG=y` and pastes one `stl` line per node.
