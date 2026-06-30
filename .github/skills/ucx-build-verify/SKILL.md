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

The current accepted obmm transport exposes one AM-only `obmm` TLS.

`obmm` should advertise:

```text
AM_SHORT
AM_BCOPY
PENDING
CONNECT_TO_IFACE
CB_SYNC
INTER_NODE
```

`UCX_OBMM_SAME_NODE_MEMID` optionally adds an internal same-node CC FIFO. It
does not register another TLS or capability set.

The TLS should not advertise:

```text
AM_ZCOPY
PUT/GET/RMA
atomics
EP_CHECK
AM_DUP
ERRHANDLE_PEER
```

Default geometry for each configured export:

```text
FIFO_SIZE       = 256
FIFO_ELEM_SIZE  = 131200
BCOPY_SEG_SIZE  = 131072
FIFO_MIN_POLL   = 64
FIFO_MAX_POLL   = 128
slot_count      = 96
required_region = 3,224,385,856 bytes = 3075.014 MiB
max_short       = 131184 total AM bytes
max_bcopy       = 131072 bytes
```

`am_short` and `am_bcopy` share the FIFO element allocation with overlapping
data ranges: short starts at byte 16 and bcopy starts at byte 64.
Dedicated short lanes are removed.

## Build Wiring Expectations

- The transport discovers shmdevs through sysfs and maps `/dev/obmm_shmdev*`
  directly. `UCX_OBMM_MEMIDS` is required.
  `UCX_OBMM_SAME_NODE_MEMID` is optional and must name exactly one export.
- libobmm headers/library are not required for the shipped transport.
- The transport must not call libobmm export/import/preimport/unpreimport or
  ownership APIs.
- `ucx_info -c | grep OBMM` should show one shared `UCX_OBMM_*` iface tuning
  group and no private cleanup-time stats/performance knobs.

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

Expected result:

- `ucx_info -d -t obmm` shows `am_short`, `am_bcopy`, pending,
  `INTER_NODE`, and no `am_zcopy`.
- `max_short` is 131184 by default.
- `max_bcopy` is 131072 by default.
- PUT/GET/RMA, atomics, and EP_CHECK remain absent.

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
- Use the earlier AM-only OSU pass as prior context, but require fresh target
  validation for new geometry or protocol-selection tuning.
