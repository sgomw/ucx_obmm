# UCX Build And Verify

Use this skill before claiming build, capability, or benchmark status for the
obmm UCT transport.

## Local Environment

- This workspace is Windows. Do not waste time trying to run Linux UCX build or
  target hardware commands locally.
- Do not run `mpirun`, `ucx_perftest`, OSU, or any two-node hardware test
  locally.
- Local verification is limited to static checks, grep-based symbol/capability
  checks, and diff review.

## Expected Transport Surface

Do not encode the expected obmm capability surface, geometry, wire format, or
configuration knobs in this skill. Read `ucx/src/uct/obmm/DESIGN.md` and verify
the target build against that document.

## Build Wiring Expectations

- Build wiring expectations are design-specific. Check source, generated
  symbols, and `ucx_info` output against `DESIGN.md` rather than this skill.
- If `DESIGN.md` says a dependency or capability is absent, verify that absence
  explicitly with grep, symbol inspection, or `ucx_info`.

## Target Verification Commands

Run these only on the Linux target/build host:

```sh
./autogen.sh
./contrib/configure-devel --prefix="$PWD/install"
make -j
make install
```

Capability checks:

```sh
UCX_TLS=obmm "$PWD/install/bin/ucx_info" -d
UCX_TLS=obmm "$PWD/install/bin/ucx_info" -d -t obmm
UCX_TLS=obmm "$PWD/install/bin/ucx_info" -c | grep OBMM
```

Expected result: compare the reported capabilities, numeric caps, config
knobs, and loaded symbols to `ucx/src/uct/obmm/DESIGN.md`.

## Protocol Selection Diagnostics

UCP protocol-selection logging is intentionally retained for obmm tuning:

```sh
UCX_PROTO_SELECT_LOG=y UCX_PROTO_SELECT_LOG_RANK=0 ...
```

The log prints one-shot `ucp_proto_select:` lines for obmm lanes without
requiring broad debug logging. When asking the user for target logs, request
only the one to three relevant lines or fields.

## Reporting Rules

- Do not claim target behavior from local static checks.
- If no Linux target/build verification was run, say so plainly.
- Treat older benchmark passes as prior context only. Require fresh target
  validation for changed design, geometry, lifecycle, or protocol-selection
  behavior.
