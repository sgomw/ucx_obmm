---
name: ucx-build-verify
description: >
  Environment-gated build and verification workflow for UCX obmm changes.
  Use after changes under ucx/src/uct/obmm/ or obmm build wiring.
---

# UCX Build & Verify (obmm)

No real OBMM hardware is available in the local workspace. The local workspace
may also be Windows-only with no bash/WSL/gcc. Do not blindly run Linux build
commands in that environment.

## First: detect the environment

Before attempting a build, check whether a Linux shell and toolchain are
available.

Build is allowed only if all are true:

- a POSIX shell can run `./autogen.sh`
- autotools/configure dependencies exist
- `make` and a C compiler exist

If not available:

- Do **not** retry `bash`, `wsl`, `gcc`, or `make` repeatedly.
- Run local static checks only:
  - `git diff --check`
  - targeted `rg` for changed symbols/call sites
  - code-review agent
- Report that compile verification must be run on the Linux build host or by
  the user.

## Build wiring facts

- obmm sources are listed directly in `ucx/src/uct/Makefile.am`.
- There is no `ucx/src/uct/obmm/Makefile.am` and no obmm `configure.m4`.
- obmm is built unconditionally as part of core UCT.
- Avoid adding a hard libobmm link dependency unless explicitly approved.
  V3 currently uses lazy symbol resolution for `obmm_set_ownership` to keep
  build wiring simple and preserve NC mode.

## Linux build commands

Run from `ucx/` on a Linux build host:

```sh
./autogen.sh
./contrib/configure-devel --prefix=$PWD/install
make -j
make install
```

Use a `task` agent for verbose Linux builds when available. If it fails, inspect
the build log and fix compile errors before proceeding.

## No-hardware verification after install

1. Component registration:

   ```sh
   ./install/bin/ucx_info -d | grep -iE "obmm|Component"
   ```

2. Capability introspection:

   ```sh
   ./install/bin/ucx_info -d -t obmm
   ```

   Check that caps match the current implementation:

   - `AM_SHORT`
   - `AM_BCOPY`
   - `PENDING`
   - `INTER_NODE`
   - `max_short`
   - `max_bcopy`

3. Config keys:

   ```sh
   ./install/bin/ucx_info -c | grep -i OBMM
   ```

   V3 should expose `MEM_MODE`, `NC_MEMIDS`, `CC_MEMIDS`, `CC_CHUNK_SIZE`, and
   existing FIFO/bcopy knobs.

4. Symbol sanity:

   ```sh
   nm -D ./install/lib/libuct.so | grep uct_obmm
   ```

## Hardware-required checks

Do not run these locally:

- `mpirun`
- OSU
- `ucx_perftest` against obmm hardware
- any two-node/hardware-dependent test

The user runs these on the real nodes. When reporting results, distinguish:

- **locally verified**: static checks, code review, Linux build if available
- **user/hardware verified**: MPI/OSU/perftest on real OBMM nodes
- **not verified**: ordering or ownership behavior that requires hardware
