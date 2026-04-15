# Copilot instructions

## Repository goal and boundaries

This workspace exists to add a new UCT shared-memory transport over the OBMM userspace API.

- `ucx/` is the implementation target.
- `obmm/` is a read-only dependency used for API and behavior reference.

Editing boundaries are strict:

- Allowed: `ucx/src/uct/obmm/**`.
- Allowed when needed for OBMM TL wiring only: the already-added shared UCT registration/build/config plumbing under `ucx/src/uct/**`.
- Not allowed: changes anywhere else in UCX.
- Not allowed: changes anywhere under `obmm/`.

Treat OBMM integration work as implementing the UCX-side transport only. Do not redesign or modify libobmm.

## Build, test, and lint commands

### UCX (`ucx/`)

Commands assume a Linux shell and should be run from `ucx/`.

| Task | Command |
| --- | --- |
| Developer configure | `./autogen.sh && ./contrib/configure-devel --prefix=$PWD/install-debug` |
| Release configure | `./autogen.sh && ./contrib/configure-release --prefix=/where/to/install` |
| Build | `make -j$(nproc)` |
| Install | `make install` |
| Build docs | `make docs` |
| Inspect discovered transports/devices | `./src/tools/info/ucx_info -d` |
| Run all gtests | `make -C test/gtest test` |
| List gtests | `make -C test/gtest list` |
| Run one test or suite | `make -C test/gtest GTEST_FILTER=test_ucp_context.* test` |
| Spelling check | `codespell` |
| Format check on a diff | `git-clang-format --diff <base> <head>` |

UCX CI also enforces the repo code style in `docs/CodeStyle.md` and logging conventions in `docs/LoggingStyle.md`.

### OBMM userspace library (`obmm/src/libobmm/`)

Commands should be run from `obmm/src/libobmm/`.

| Task | Command |
| --- | --- |
| Configure | `cmake -S . -B build` |
| Build | `cmake --build build` |
| Install | `cmake --install build` |
| Coverage-style build | `cmake -S . -B build-gcov -DGCOV=ON && cmake --build build-gcov` |

Use these only to understand the dependency or validate an external environment. Do not change OBMM sources in this repository.

## High-level architecture

### UCX

UCX is layered in the standard UCS/UCM/UCT/UCP stack, and the build graph in `ucx/Makefile.am` reflects that:

- `UCS` provides shared utilities, data structures, config parsing, stats, memory helpers, and class/macros used everywhere else.
- `UCM` provides memory-event interception and related hooks.
- `UCT` is the transport layer. Components register memory domains and transport implementations, and each transport exposes its iface/ep operations through the UCT class and registration macros.
- `UCP` is the high-level protocol layer. It discovers all UCT components, opens their memory domains, collects transport resources, builds registration/cache maps, and then workers/endpoints choose lanes and protocols from that resource graph.

The key files for that flow are:

- `ucx/src/uct/base/uct_component.h` and `ucx/src/uct/base/uct_component.c` for component registration and discovery.
- `ucx/src/uct/base/uct_iface.h` for transport registration macros such as `UCT_TL_DEFINE`.
- `ucx/src/ucp/core/ucp_context.c` for `uct_query_components()` -> MD open -> transport resource collection -> UCP context setup.

### UCX OBMM transport

The active feature work is in `ucx/src/uct/obmm/`, which is already wired into UCT discovery:

- `ucx/src/uct/obmm/base/obmm_md.c` defines `uct_obmm_component`, a single MD, and current MD capabilities.
- `ucx/src/uct/obmm/base/obmm_iface.c` registers TL `obmm`, reuses `uct_sm_base_query_tl_devices()`, and exposes iface discovery/query logic.
- `ucx/src/uct/obmm/base/obmm_ep.c` provides the endpoint shell and connectivity checks.
- `ucx/src/uct/base/uct_component.c` already calls `UCT_TL_DECL(obmm)` and includes the OBMM iface header so the TL appears in normal component enumeration.
- `ucx/src/uct/Makefile.am` already builds the OBMM MD/iface/ep sources into `libuct`.

Current state: the TL is intentionally minimal and should be treated as scaffolding. `ucx_info -d` can discover it, but the data path is not implemented yet:

- MD registration/deregistration still uses dummy handlers.
- RKEY unpack is placeholder logic.
- EP AM/PUT/GET/atomic operations are all still unsupported stubs.
- Reachability and endpoint creation exist only to support enumeration and basic connection checks.

### OBMM

OBMM’s userspace side is a thin ABI wrapper over the kernel module rather than a large standalone subsystem:

- `obmm/src/libobmm/libobmm.h` defines the public ABI (`struct obmm_mem_desc`, `struct obmm_preimport_info`, export/import/preimport/ownership/query functions).
- `obmm/src/libobmm/libobmm.c` opens `/dev/obmm`, fills ioctl command structs from those descriptors, and exposes the exported library entry points.
- `obmm/src/libobmm/vendor_adaptor.c` is the platform-specific bridge. It reads UB controller state from `/sys/devices/ub_bus_controller*/...`, derives vendor info for export, and validates/fixes import and preimport commands using EID/CNA/NUMA metadata.

The operational flow is documented in `obmm/doc/libobmm.md` and mirrored in the code: export on the provider, import or preimport on the consumer, access the memory, then unimport before unexport.

## Key conventions

### UCX-specific

- Follow UCX naming prefixes from `docs/CodeStyle.md`: functions and types are expected to stay within the `ucp_`, `uct_`, `ucs_`, or `ucm_` families, with matching uppercase macro prefixes.
- UCX is macro-heavy and registration-driven. Before changing behavior, read enough surrounding context to understand the macro expansion path, class hierarchy, callback hooks, and registration flow rather than reasoning from a single file in isolation.
- New UCT transports should be registered with the existing macro/class system (`UCT_TL_DEFINE`, `UCT_TL_DEFINE_ENTRY`, `UCT_TL_INIT`, `UCS_CLASS_*`) instead of hand-rolled registration paths.
- UCP transport selection is resource-driven. If you change UCT components, MD capabilities, or TL registration, expect the real integration point to be `ucp_context.c`, not just the local transport file.
- UCX logging is intentionally stylized: lowercase messages, avoid `key=value`, and log the concrete failure at the first layer that actually knows the cause.
- The main unit-test binary is `ucx/test/gtest/gtest`; use `GTEST_FILTER` through `make -C test/gtest ...` instead of inventing a separate single-test workflow.
- For this repo, keep implementation work isolated to the OBMM transport and its existing UCT wiring. Do not spread OBMM-specific behavior into unrelated UCX transports or UCP.
- Prefer existing SHM transports as structural references for iface/ep/MD behavior, but keep the actual implementation rooted in OBMM semantics and libobmm capabilities.
- When implementing the OBMM TL, always compare against neighboring transport implementations before inventing new patterns, especially around MD lifecycle, iface caps, endpoint connectivity, rkey flow, and progress/flush semantics.

### OBMM-specific

- `libobmm` is intentionally thin. Preserve the ioctl-oriented design instead of wrapping the kernel ABI in extra stateful abstractions.
- `struct obmm_mem_desc` and `struct obmm_preimport_info` use flexible-array tails for `priv`; callers are expected to allocate `sizeof(struct ...) + priv_len` bytes and keep `priv_len` zero when unused.
- Ownership is expressed through memory protection bits in `obmm_set_ownership()` (`PROT_NONE`, `PROT_READ`, `PROT_WRITE`), matching the shared-memory consistency model in the docs.
- Changes around export/import/preimport usually span both `libobmm.c` and `vendor_adaptor.c`: descriptor fields come from the public ABI, but EID/CNA/NUMA validation and vendor payload generation happen in the adaptor.
- In this repo, OBMM is reference material. Read `obmm/doc/*.md`, `obmm/src/libobmm/libobmm.h`, `obmm/src/libobmm/libobmm.c`, and `obmm/src/libobmm/vendor_adaptor.c` to understand the contract, then implement the UCX side without modifying the dependency.
- Use the local Chroma vector DB to reduce hallucinations when the UCX context fan-out is too large: `.\.artifacts\chromadb` contains `ucx_code` and `obmm_code` collections. Prefer the hit metadata (`path`, `root`, `start_line`, `end_line`) to narrow code retrieval toward `ucx/src`, `obmm/src/libobmm`, or `obmm/doc` before opening many raw files.
