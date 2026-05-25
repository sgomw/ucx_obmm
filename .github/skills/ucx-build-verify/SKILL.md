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

## Current shipped status

- The in-tree obmm transport now registers three TLs:
  - `obmm` for the NC remote/eager role, advertising
    `AM_SHORT`, `AM_BCOPY`, `PENDING`, `CONNECT_TO_IFACE`, `CB_SYNC`, and
    `INTER_NODE`
  - `obmm_cc` for the CC local/eager role, advertising the same AM/pending
    surface except `INTER_NODE`
  - `obmm_cc_bulk` for the inter-node CC bulk role, advertising
    `AM_BCOPY`, `PENDING`, `CONNECT_TO_IFACE`, `CB_SYNC`, and `INTER_NODE`
- It does **not** currently advertise:
  `AM_ZCOPY`, PUT/GET/RMA, atomics, or `EP_CHECK`.
- The long-running correctness baseline is still the NC eager path. The newer
  `obmm_cc` and `obmm_cc_bulk` TLs are staged hybrid work and are not
  considered validated merely because the CC regions map and `ucx_info` lists
  them.
- The current baseline has already passed the full OSU micro-benchmark suite
  on the real two-node setup. That hardware result is the repository's
  correctness baseline for the currently advertised capabilities, but it
  cannot be re-run from this Windows workspace.

## Build wiring (current state)

- The obmm sources are listed directly in `ucx/src/uct/Makefile.am`:
    `obmm/base/obmm_md.{c,h}`, `obmm_iface.{c,h}`, `obmm_ep.{c,h}`,
    `obmm_bulk.h`, and `obmm_ownership.{c,h}`.
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
   Expect to see a `Component: obmm` block listing the obmm md and all
   configured TLs. With CC memids configured, that should include
   `obmm`, `obmm_cc`, and `obmm_cc_bulk`.

2. obmm capabilities reflect the current transport surface:
   ```
   ./install/bin/ucx_info -d -t obmm
   ```
   Confirm the tl block shows `am_short`, `am_bcopy`, and iface flags
   matching the current baseline rather than an older placeholder state with
   zero AM caps.

   If `UCX_OBMM_CC_MEMIDS` is configured, also check:
   ```
   ./install/bin/ucx_info -d -t obmm_cc
   ```
   Confirm that the CC-local TL is listed and that it does **not** advertise
   `INTER_NODE`.

   And:
   ```
   ./install/bin/ucx_info -d -t obmm_cc_bulk
   ```
   Confirm that the bulk TL is listed, advertises `am_bcopy` but not
   `am_short`, and still requires `INTER_NODE`.

3. Config keys are exposed:
   ```
   ./install/bin/ucx_info -c | grep -i OBMM
   ```
   For the current MD/config groundwork, expect `OBMM_NC_MEMIDS` and
   `OBMM_CC_MEMIDS`. The legacy `OBMM_MEMIDS` key should not be present.

4. Symbol sanity:
   ```
   nm -D ./install/lib/libuct.so | grep uct_obmm
   ```
   Look for `uct_obmm_component`, `uct_obmm_iface_t_*`,
   `uct_obmm_ep_am_short`, `uct_obmm_ep_am_bcopy`, and the
   `obmm_cc_bulk` TL registration symbols.

## Hardware validation baseline

- Full transport correctness for the current AM-only baseline has already been
  established on the real two-node target by passing the full OSU
  micro-benchmark suite.
- Use that result as a baseline when reviewing new performance work, but do
  not present it as if it were re-validated by the current local session.
- Any change to capabilities, wire format, ownership semantics, reachability,
  or transport geometry still needs fresh two-node validation after it builds.
- If you changed MD/config behavior, also confirm the expected negative path:
  `ucx_info -d -t obmm` should fail clearly when the required
  `UCX_OBMM_NC_MEMIDS` configuration is missing.

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
