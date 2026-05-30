# CLAUDE.md — ucx_obmm_br

This is the project memory for the **obmm UCT transport** development repository.
It is initialized from the full source tree, DESIGN.md, AGENTS.md, and the four
project skills loaded into every session.

## Project identity

- **Goal**: Implement and maintain a UCX UCT transport (`obmm`) for OBMM
  shared-memory hardware, targeting a two-node ARM64 cluster.
- **Repos**: `ucx/` (active transport dev), `obmm/` (libobmm — read-only),
  `ompi/` (MPI — read-only).
- **Active code**: `ucx/src/uct/obmm/base/` — all transport logic lives here.
- **Branch**: `er` (main: `main`).

## Current baseline (v2 — validated on hardware)

- **Capabilities advertised**: `AM_SHORT`, `AM_BCOPY`, `PENDING`,
  `CONNECT_TO_IFACE`, `CB_SYNC`, `INTER_NODE`.
- **NOT advertised**: `AM_ZCOPY`, PUT/GET/RMA, atomics, `EP_CHECK`, `AM_DUP`.
- **am_short path**: Dedicated SPSC short lanes (64 lanes, 256B elements, 8-deep
  FIFO per lane). Determinisitic lane assignment (indices 0–31 local senders,
  32–63 peer senders). Max short = 248 bytes (header + payload).
- **am_bcopy path**: Legacy shared FIFO (64-deep ring) with paired desc area
  (`BCOPY_SEG_SIZE` = 32768 bytes per desc). CAS-based head reservation.
- **Pending**: `ucs_arbiter_t` with per-ep `arb_group`, mirrors mm pattern.
- **Hardware validation**: Full OSU micro-benchmark suite passed on the real
  two-node setup. This is the correctness baseline.

## Locked-in design decisions (do NOT change without re-asking)

1. **NC mapping** (`O_RDWR | O_SYNC`, `MAP_SHARED`) — mandatory. Cacheable
   mappings violate OBMM consistency model for concurrent read/write FIFO.
2. **No `obmm_export/import/preimport/unexport/unimport` calls from UCT** —
   lifecycle is handled externally. UCT opens `/dev/obmm_shmdev${memid}` and
   mmaps only.
3. **No `obmm_set_ownership`** — irrelevant with NC mappings.
4. **LSE atomics only** on aarch64 NC shared control words. Compiler-default
   LL/SC atomics are unusable on this hardware.
5. **Bus-domain fences** (`dmb oshst`/`dmb oshld`, `mfence`) not CPU-domain
   (`dmb ishst`/`dmb ishld`). CPU fences are inner-shareable only.
6. **Two-phase pool init**: UNINIT → INITING (CAS claim) → fill geometry →
   bus fence → READY. Losers spin on READY + bus load fence.
7. **Reachability keyed on `(exporter_dcna, exporter_deid, memid)`**, never
   memid alone.
8. **Slot lifecycle with generation tokens**: `(owner_pid, owner_starttime,
   generation, state)`. Crash recovery via `/proc/<pid>/stat` starttime.
9. **64-byte-aligned** `FIFO_ELEM_SIZE` and `BCOPY_SEG_SIZE` — non-aligned
   strides regress latency on the current platform.
10. **No `rkey_ptr`** — obmm cannot expose remote user buffers as local
    pointers. Only the pre-exported FIFO region is mmap'd.

## Environment constraints

- **Two nodes** (node 0, node 1), each with one pre-exported 128 MiB region.
- **ARM64 production ISA**. NC + bus fences + LSE atomics.
- **No hardware in dev environment**. Local verification is limited to:
  - `make` / `make install`
  - `ucx_info -d -t obmm`
  - `ucx_info -c | grep OBMM`
  - `nm -D libuct.so | grep uct_obmm`
  - Static review
- **Do NOT run** `mpirun`, `ucx_perftest`, or any two-node test locally.

## Source file map

| File | Role |
|------|------|
| `ucx/src/uct/obmm/DESIGN.md` | Wire format, pool layout, sync rules — **single source of truth** |
| `ucx/src/uct/obmm/base/obmm_md.{c,h}` | Memory domain: sysfs discovery, region open/mmap, export/import lookup |
| `ucx/src/uct/obmm/base/obmm_iface.{c,h}` | Interface: pool attach, slot alloc, progress (short lanes + legacy FIFO + pending dispatch), iface_query caps, reachability v2 |
| `ucx/src/uct/obmm/base/obmm_ep.{c,h}` | Endpoint: am_short (SPSC), am_bcopy (CAS reserve + paired desc), pending_add, pending_purge, is_connected |
| `ucx/src/uct/obmm/base/obmm_pool.{c,h}` | Cross-process slot allocator: two-phase init, CAS-based claim, crash scavenge, zero-on-exit reset |
| `ucx/src/uct/obmm/base/obmm_fifo.h` | FIFO ctl (head/tail cacheline-padded), short lane layout, element header, slot stride calc, `uct_obmm_bus_full_fence()` |
| `ucx/src/uct/obmm/base/obmm_atomic.h` | LSE CAS for aarch64 NC mappings; falls back to `ucs_atomic_cswap*` on other archs |
| `ucx/src/uct/obmm/base/obmm_sysfs.{c,h}` | `/sys/devices/obmm/obmm_shmdev*/` discovery, export/import_info parsing, MEMIDS allow-list |
| `ucx/src/uct/obmm/base/obmm_region.{c,h}` | `open(O_RDWR\|O_SYNC)` + `mmap(MAP_SHARED)` wrapper, close/munmap |
| `ucx/src/uct/obmm/base/obmm_stats.{c,h}` | Baseline + 1B-short perf counters and dump functions; extracted from obmm_iface |

## Key data-path helpers

- `uct_obmm_fifo_has_space()` — shared "refresh tail with bus-fence" check used by legacy FIFO CAS reserve, pending_add, and SPSC short-lane send. Eliminates the former triplicate head/tail check.
- `uct_obmm_bus_full_fence()` — `dmb osh` / `mfence` for receiver tail publish (must order prior loads before the tail store).
- `uct_obmm_atomic_cswap64()` — LSE CAS on aarch64, `ucs_atomic_cswap64` elsewhere.

## Build wiring

- obmm sources are listed directly in `ucx/src/uct/Makefile.am` (both
  `noinst_HEADERS` and `libuct_la_SOURCES`).
- No separate `configure.m4` or `Makefile.am` under `ucx/src/uct/obmm/`.
- obmm is built unconditionally as part of `libuct.la`.
- Build commands (run from `ucx/`):
  ```
  ./autogen.sh
  ./contrib/configure-devel --prefix=$PWD/install
  make -j
  make install
  ```

## Configuration knobs (`UCX_OBMM_*`)

| Knob | Default | Meaning |
|------|---------|---------|
| `BW` | 3400MBs | Transport bandwidth for UCP cost modeling |
| `FIFO_SIZE` | 64 | Ring depth (power of 2) |
| `FIFO_ELEM_SIZE` | 64 | Legacy FIFO elem metadata stride (bytes) |
| `BCOPY_SEG_SIZE` | 32768 | Per-desc bcopy area → raw `max_bcopy` |
| `FIFO_MIN_POLL` | 16 | Min RX completions per progress() |
| `FIFO_MAX_POLL` | 16 | Max RX completions per progress() |
| `PENDING_QUOTA` | 1 | Pending retries per progress() |
| `SHORT_PERF_STATS` | n | Dump 1B am_short timing on cleanup |
| `MEMIDS` | "" | Comma-separated explicit shmdev memids |

## Skills (loaded every session)

1. **`obmm-api-and-env`** — libobmm API, test topology, locked-in decisions.
2. **`uct-transport-patterns`** — UCX framework contracts, reference transports,
   mandatory consistency rules (ops table, iface_query, reachability).
3. **`ucx-build-verify`** — Build commands, no-hardware verification checklist,
   what NOT to run.
4. **`vector-db-retrieval`** — Chroma DB query workflow for UCX/OBMM/OMPI code.

## Mandatory workflow (from AGENTS.md)

1. **Retrieve before reasoning** — query vector DB before making code-grounded
   UCX/OBMM/OMPI conclusions.
2. **Re-read environment facts** — check `obmm-api-and-env` before touching
   memory setup, mmap, ownership, addressing, reachability.
3. **Diagnose before changing code** — locate evidence (error strings, stack
   traces, progress state) before modifying.
4. **Plan and critique** — update plan.md for design changes; run design
   critique for ops/capability/wire-format/ownership changes.
5. **Implement coherently** — keep ops table, iface_query, address structs,
   reachability, pool geometry, and DESIGN.md in sync.
6. **Verify** — follow `ucx-build-verify` checklist.
7. **Code-review** — run high-signal review on diff before declaring done.

## Hard rules (from AGENTS.md)

- Do NOT call `obmm_export/unexport/import/unimport/preimport/unpreimport` from UCT.
- Do NOT call `obmm_set_ownership`.
- Do NOT run `mpirun`, `ucx_perftest`, or hardware tests locally.
- Do NOT modify `ompi/` or `obmm/` without explicit ask.
- Do NOT modify files outside `ucx/src/uct/obmm/`, `ucx/src/uct/Makefile.am`,
  and needed build wiring without approval.
- Do NOT advertise a capability unless the operation is truly implemented.
- Do NOT use memid as a cross-node peer key.
- Do NOT use generic compiler-lowered atomics on arm64 NC mappings.
- Do NOT invent libobmm semantics, device paths, mmap offsets, or cache
  coherence behavior.

## Test artifacts (root directory)

- `mpi_correctness_v2.c`, `mpi_pingpong_v2.c`, `mpi_bw_v2.c`, `mpi_multi_v2.c` — V2 MPI tests (cross am_short/bcopy boundaries)
- `mpi_correctness.c`, `mpi_pingpong.c`, `mpi_bw.c`, `mpi_collective.c`, `mpi_sanity.c` — V1 tests (am_short range only)
- `build_mpi_tests.sh`, `run_mpi_tests.sh`, `run_osu_tests.sh`, `run_osu_baseline.sh`
- `obmm_pool_reset.c` — manual pool reset utility
- `cc_nc_probe.c` — NC mapping atomic/latency characterization probe
- `obmm_posix_same_node_probe.c` — same-node POSIX shared memory probe
- `test_cast.c` — type casting test
