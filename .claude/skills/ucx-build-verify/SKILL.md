---
name: ucx-build-verify
description: >
  How to build UCX with the obmm transport and how to verify it without
  hardware.  Use after any change under ucx/src/uct/obmm/ or to the obmm
  build wiring.
---

# UCX Build & Verify (obmm)

No real obmm hardware or Linux toolchain is available in this environment.
Verification is limited to static review.  Do not attempt to run perftests
or MPI jobs locally.

## Current status

- The in-tree obmm transport is the **legacy NC baseline** (validated, NC FIFO design).
- The active task is a **CC-only redesign** that replaces it.

## Build wiring

- obmm sources listed in `ucx/src/uct/Makefile.am` (noinst_HEADERS + libuct_la_SOURCES).
- No separate `configure.m4` or `Makefile.am` under `ucx/src/uct/obmm/`.
- obmm is built unconditionally as part of `libuct.la`.
- libobmm is NOT yet wired into UCX's configure.  If the CC transport needs
  `#include <obmm/...>` or `-lobmm`, a `configure.m4` must be added.

## Build commands

Build occurs on the Linux two-node setup, not in the dev workspace.
For reference, from `ucx/`:

```
./autogen.sh
./contrib/configure-devel --prefix=$PWD/install
make -j
make install
```

## Hardware validation

- Legacy NC baseline: full OSU micro-benchmark suite passed on two-node setup.
- CC transport: must pass correctness tests on the real two-node setup.
- Any change to capabilities, wire format, ownership semantics, or reachability
  needs fresh two-node validation.

## What NOT to run

- `mpirun`, `ucx_perftest`, or any two-node test locally — no hardware.
- Any test that requires a second node or the obmm kernel module.
- `make`, `ucx_info`, `nm` — no Linux toolchain in this workspace.

## When verification is impossible

If a change cannot be exercised locally, state that explicitly and flag it as a
hardware-required follow-up.  Do not fabricate "passed" results.
