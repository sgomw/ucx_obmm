---
name: ucx-build-verify
description: >
  How to build UCX with the obmm transport and how to verify it without
  hardware. Use after any change under ucx/src/uct/obmm/ or to the obmm
  build wiring.
---

# UCX Build & Verify (obmm)

No real obmm hardware is available in this environment. Verification is
therefore limited to a successful build plus introspection via
`ucx_info`. Do not attempt to run perftests or MPI jobs.

## Build wiring (current state)

- The obmm sources are listed directly in `ucx/src/uct/Makefile.am`:
    `obmm/base/obmm_md.{c,h}`, `obmm_iface.{c,h}`, `obmm_ep.{c,h}`.
- There is currently **no** `ucx/src/uct/obmm/configure.m4` and **no**
  `ucx/src/uct/obmm/Makefile.am`. obmm is built unconditionally as part
  of the core uct library.
- libobmm headers / library are NOT yet wired into UCX's configure. If the
  am_short implementation needs to `#include <obmm/...>` or link against
  `libobmm`, add a `configure.m4` under `ucx/src/uct/obmm/` and an
  `AC_CONFIG_FILES` entry in `ucx/configure.ac`, mirroring how
  `ucx/src/uct/sm/mm/xpmem/configure.m4` and
  `ucx/src/uct/cuda/` do it. Confirm with the user before adding a hard
  dependency on libobmm — the test environment may not have it
  installed where UCX expects.

## Build commands

Run from `ucx/`:

```
./autogen.sh
./contrib/configure-devel --prefix=$PWD/install   # or configure-release
make -j
make install
```

Use the `task` agent to run these so verbose output is summarized. On
failure, inspect the full log it returns.

## No-hardware verification checklist

After `make install`, run these and confirm:

1. obmm component is registered:
   ```
   ./install/bin/ucx_info -d | grep -iE "obmm|Component"
   ```
   Expect to see a `Component: obmm` block listing the obmm md and the
   obmm tl.

2. obmm capabilities reflect the current receiver-local atomic FIFO baseline:
   ```
   ./install/bin/ucx_info -d -t obmm
   ```
   Confirm the tl block shows:
   - `am_short: 16432`
   - `am_bcopy: 32768`
   - iface flags including `AM_SHORT`, `AM_BCOPY`, `PENDING`, and
     `INTER_NODE`

3. Config keys are exposed:
   ```
   ./install/bin/ucx_info -c | grep -i OBMM
   ```
   Confirm the output includes `OBMM_SHARD_COUNT=8` and `OBMM_FIFO_SIZE=4`
   defaults alongside the existing geometry knobs.

4. Symbol sanity:
   ```
   nm -D ./install/lib/libuct.so | grep uct_obmm
   ```
   Look for `uct_obmm_component`, `uct_obmm_iface_t_*`, and the new
   `uct_obmm_ep_am_short` / `uct_obmm_ep_am_bcopy`.

## Current runtime status (from user hardware runs)

- In this workspace we still cannot run hardware validation locally.
- On the user's target setup, the earlier sender-owned mailbox baseline passed
  the OSU point-to-point suite and the OSU collective suite.
- The current receiver-local sharded atomic FIFO redesign has not yet been
  hardware-validated in this workspace.

## What NOT to run

- `mpirun`, `ompi_info`, `ucx_perftest -t am_short` against obmm — no
  hardware, will fail or hang.
- Any test that requires a second node — there is no second node in this
  workspace, only the description of one.

## When verification is impossible

If a change cannot be exercised by the checks above (e.g. wire-format
detail, FIFO ordering on real hardware), state that explicitly in the
final report and flag it as a hardware-required follow-up. Do not
fabricate "passed" results.
