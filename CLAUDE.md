# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

A three-repo integration tree for developing a custom UCX UCT transport (`obmm`) against Huawei OBMM (ownership-based memory management) hardware. The transport enables cross-node active-message communication over a Unified Bus interconnect using non-cacheable (NC) memory mappings.

- `ucx/` — active development repo; the obmm transport lives in `ucx/src/uct/obmm/base/`
- `obmm/` — libobmm API and sysfs/runtime semantics; **read-only context, do not extend**
- `ompi/` — Open MPI build configured with PML=ucx; **read-only context**

## Mandatory reading before non-trivial work

- `AGENTS.md` — mandatory workflow, scope, hard rules
- `ucx/src/uct/obmm/DESIGN.md` — wire format, pool layout, fence rules, config knobs
- `.github/skills/obmm-api-and-env/SKILL.md` — libobmm API facts, locked-in design decisions
- `.github/skills/uct-transport-patterns/SKILL.md` — UCX framework contracts, reference transports
- `.github/skills/ucx-build-verify/SKILL.md` — build and no-hardware verification
- `.github/skills/vector-db-retrieval/SKILL.md` — query the local Chroma vector DB

## Build commands (Linux build host only — NOT this workspace)

This Windows workspace has no Linux shell, no toolchain, and no obmm hardware.
All build and verification commands below must run on a Linux build host or be
handed to the user.

### UCX (developer build)

```bash
cd ucx
./autogen.sh
./contrib/configure-devel --prefix=$PWD/install
make -j
make install
```

### UCX (release build)

```bash
cd ucx
./autogen.sh
./contrib/configure-release --prefix=/path/to/install
make -j
make install
```

### Post-build verification (Linux build host, after install)

```bash
./install/bin/ucx_info -d -t obmm          # confirm component + caps
./install/bin/ucx_info -c | grep -i OBMM   # confirm config keys
nm -D ./install/lib/libuct.so | grep uct_obmm  # confirm symbols
make -C test/gtest test                    # UCX unit tests
```

### What IS possible in this workspace (static only)

- Code review, grep, cross-referencing source files
- Verifying ops tables, capability flags, address structs, reachability, and
  config tables are internally consistent
- Checking Makefile.am lists every source file
- Confirming symbols referenced in code exist in the declared headers

## Architecture

### Data path (sender → receiver)

```
MPI App → Open MPI PML UCX → UCP protocol layer → UCT iface → obmm transport
                                                                  ↓
                                                        NC mmap (O_SYNC)
                                                                  ↓
                                                     /dev/obmm_shmdev*
                                                                  ↓
                                                         Unified Bus → remote node
```

### obmm transport component breakdown

Each pair of source files in `ucx/src/uct/obmm/base/` handles one concern:

| Component | Files | Responsibility |
|-----------|-------|----------------|
| MD (memory domain) | `obmm_md.c/h` | Discover shmdev devices via sysfs, mmap export/import regions, lifecycle |
| IFACE (interface) | `obmm_iface.c/h` | Capabilities, reachability, progress loop, receive-side FIFO drain |
| EP (endpoint) | `obmm_ep.c/h` | Send path: `am_short` (SPSC lanes), `am_bcopy` (paired descriptors), pending queue |
| FIFO | `obmm_fifo.h` | Wire format: control headers, short lanes, element layout, bus-fence macros |
| Pool | `obmm_pool.c/h` | Two-phase pool init (UNINIT→INITING→READY via CAS), slot allocation, generation tracking |
| Region | `obmm_region.c/h` | `open(O_RDWR\|O_SYNC)` + `mmap` a single shmdev region |
| Sysfs | `obmm_sysfs.c/h` | Scan `/sys/devices/obmm/obmm_shmdev*/` for exporter/importer discovery |
| Atomic | `obmm_atomic.h` | arm64 LSE explicit CAS (32/64-bit), fallback to generic `ucs_atomic_*` on x86 |

### Sender flow

1. Reserve a slot in peer's receive FIFO (CAS on head)
2. For `am_short`: write inline to deterministic SPSC lane; for `am_bcopy`: write to paired `desc[N]`
3. Fill FIFO element metadata (flags, am_id, length, generation)
4. `bus_store_fence` (release)
5. Publish flags byte

### Receiver flow (`iface_progress`)

1. Drain SPSC short lanes, then drain legacy FIFO
2. On new slot: `bus_load_fence` (acquire), validate generation
3. Dispatch via `uct_iface_invoke_am()` with either inline short buffer or paired desc
4. Advance tail with `bus_full_fence` before store

### UCX framework contracts (must stay consistent)

When modifying transport code, keep these in sync:
- **ops tables**: `uct_iface_ops_t`, `uct_iface_internal_ops_t`, `uct_md_ops_t`
- **capability flags** in `iface_query()` plus numeric caps (`max_short`, `max_bcopy`, etc.)
- **iface/device address structs** (`uct_obmm_iface_addr_t`, `uct_obmm_device_addr_t`)
- **reachability** (`iface_is_reachable_v2` — keys on exporter identity, not memid alone)
- **`DESIGN.md`** — update before changing layout, capabilities, or sync rules

### Key UCX helper macros

- `UCT_TL_DEFINE_ENTRY` / `UCT_SINGLE_TL_INIT` — component registration
- `UCS_CLASS_*` — class hierarchy (INIT/CLEANUP/DEFINE)
- `uct_iface_invoke_am()` — dispatch to upper layer
- `UCT_CHECK_AM_ID()`, `UCT_CHECK_LENGTH()` — validation

## Current capability baseline

**Advertised**: `AM_SHORT`, `AM_BCOPY`, `PENDING`, `CONNECT_TO_IFACE`, `CB_SYNC`, `INTER_NODE`

**NOT advertised**: PUT/GET/RMA, atomics, `AM_ZCOPY`, `EP_CHECK`, `AM_DUP`, `ERRHANDLE_PEER`

The baseline has passed the full OSU micro-benchmark suite on real two-node hardware. PUT/GET/RMA/zcopy remain unsupported unless a separate design is approved.

## Hard constraints

- **Never call** `obmm_export`, `obmm_unexport`, `obmm_import`, `obmm_unimport`, `obmm_preimport`, `obmm_unpreimport`, or `obmm_set_ownership` from the UCT transport
- **NC mapping only**: `open(O_RDWR | O_SYNC)` + `mmap MAP_SHARED` — cacheable mappings are incompatible with the FIFO usage pattern
- **arm64 atomics**: shared control-word RMW must use explicit LSE instructions; compiler-default LL/SC is unusable on NC mappings
- **Bus-domain fences**: use `ucs_memory_bus_store_fence()` / `ucs_memory_bus_load_fence()` / `uct_obmm_bus_full_fence()` — CPU-domain fences (`ucs_memory_cpu_*`) do not cover cross-host NC visibility
- **Reachability key**: `(exporter_dcna, exporter_deid, memid)` — never memid alone
- **Peer identity**: match by exporter identity, not memid
- **Geometry**: keep `FIFO_ELEM_SIZE` and `BCOPY_SEG_SIZE` 64-byte aligned unless new measurements prove otherwise
- **Do not modify** `ompi/` or `obmm/` unless explicitly asked
- **Do not run** `mpirun`, `ucx_perftest`, or any two-node test locally — this is a Windows analysis-only workspace

## Reference transports (for patterns, not blind copying)

- `ucx/src/uct/sm/mm/base/` — FIFO-style AM transport, pending wiring
- `ucx/src/uct/sm/self/` — minimal AM/endpoint wiring
- `ucx/src/uct/tcp/` — inter-node AM bcopy/zcopy
- `ucx/src/uct/ib/rc/base/`, `ucx/src/uct/ib/ud/base/` — capability exposure, perf modeling

## Configuration knobs (all `UCX_OBMM_*`)

| Knob | Default | Meaning |
|------|---------|---------|
| FIFO_SIZE | 64 | Ring depth (power of 2) |
| FIFO_ELEM_SIZE | 64 | Legacy FIFO metadata stride |
| BCOPY_SEG_SIZE | 32768 | Paired desc size → raw max_bcopy |
| PENDING_QUOTA | 1 | Pending retries per progress() |
| MEMIDS | "" | Comma-separated explicit shmdev memids (disables sysfs scan) |

Progress is fixed-budget: each `progress()` call drains up to 16 receive completions before yielding. Bandwidth for UCP lane cost modeling is hardcoded at 3400 MB/s.
| MEMIDS | "" | Comma-separated explicit shmdev memids (disables sysfs scan) |
