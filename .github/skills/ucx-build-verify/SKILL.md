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

The current accepted obmm transport is dual-plane and AM-only.

`obmm_nc` should advertise:

```text
AM_SHORT
AM_BCOPY
PENDING
CONNECT_TO_IFACE
CB_SYNC
INTER_NODE
```

`obmm_cc` should advertise the same AM/pending/connect capabilities without
`INTER_NODE`; it is same-node only.

Neither plane should advertise:

```text
AM_ZCOPY
PUT/GET/RMA
atomics
EP_CHECK
AM_DUP
ERRHANDLE_PEER
```

Default NC/CC geometry:

```text
FIFO_SIZE       = 64
FIFO_ELEM_SIZE  = 520128
BCOPY_SEG_SIZE  = 4096
slot_count      = 96
required_nc     = 3,220,846,912 bytes = 3071.639 MiB
max_short       = 520112 total AM bytes
max_bcopy       = 4096 bytes
```

`am_short` and `am_bcopy` share the FIFO. Dedicated short lanes are removed.

## Build Wiring Expectations

- The transport discovers shmdevs through sysfs and maps `/dev/obmm_shmdev*`
  directly. Users classify planes with `UCX_OBMM_NC_MEMIDS` and
  `UCX_OBMM_CC_MEMIDS`; explicit lists are discovered together and then
  classified.
- libobmm headers/library are not required for the shipped transport.
- The transport must not call libobmm export/import/preimport/unpreimport or
  ownership APIs.
- `ucx_info -c | grep OBMM` may show `UCX_OBMM_CC_*` same-node CC knobs, but
  should not show private cleanup-time stats/performance knobs.

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
UCX_TLS=obmm_nc,obmm_cc "$PWD/install/bin/ucx_info" -d
UCX_TLS=obmm_nc,obmm_cc "$PWD/install/bin/ucx_info" -d -t obmm_nc
UCX_TLS=obmm_nc,obmm_cc "$PWD/install/bin/ucx_info" -d -t obmm_cc
UCX_TLS=obmm_nc,obmm_cc "$PWD/install/bin/ucx_info" -c | grep OBMM
```

Expected result:

- `ucx_info -d -t obmm_nc` shows `am_short`, `am_bcopy`, pending,
  `INTER_NODE`, and no `am_zcopy`.
- `ucx_info -d -t obmm_cc` shows `am_short`, `am_bcopy`, pending, no
  `INTER_NODE`, and no `am_zcopy`.
- `max_short` is 520112 by default.
- `max_bcopy` is 4096 by default.
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
