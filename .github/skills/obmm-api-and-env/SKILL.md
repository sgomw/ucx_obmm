# OBMM API And Environment

Use this skill before touching obmm memory setup, mmap flags, sysfs discovery,
reachability, ownership assumptions, or hardware/topology facts.

## Current Transport Scope

- Active UCX transport work is in `ucx/src/uct/obmm/`.
- The target transport is dual-plane, AM-only, and short-first.
- It registers `obmm_nc` for cross-node NC AM and `obmm_cc` for same-node
  cacheable CC AM under the same `obmm` component.
- `obmm_nc` advertises `AM_SHORT`, `AM_BCOPY`, `PENDING`,
  `CONNECT_TO_IFACE`, `CB_SYNC`, and `INTER_NODE`.
- `obmm_cc` advertises the same AM/pending/connect capabilities but not
  `INTER_NODE`; it is reachable only for peers on the same local CC export.
- The TLS can run standalone: `obmm_cc` is CC-only and same-node-only, while
  `obmm_nc` uses local NC export loopback for same-node peers only when this MD
  has no local CC export.
- Neither plane advertises `AM_ZCOPY`, PUT/GET/RMA, atomics, `EP_CHECK`,
  AM_DUP, or ERRHANDLE_PEER.
- Cross-node cacheable CC as a UCT transport data path was explored and
  rejected on 2026-06-05. Do not implement or tune staged CC zcopy,
  sender-owned CC, receiver-owned CC, or CC batch/epoch paths unless the user
  explicitly opens a new design.

## Environment Facts

1. Each node currently has one externally exported 3 GiB NC region.
2. Each node imports the peer node's 3 GiB NC region before UCX/MPI starts.
3. The transport discovers regions through
   `/sys/devices/obmm/obmm_shmdev*/{export_info,import_info}` and maps
   `/dev/obmm_shmdev*` directly.
4. UCT must not call `obmm_export`, `obmm_unexport`, `obmm_import`,
   `obmm_unimport`, `obmm_preimport`, or `obmm_unpreimport`.
5. UCT must not call `obmm_set_ownership()`. NC does not need it, same-node CC
   direct AM does not need it, and the cross-node cacheable CC route is
   rejected.
6. Do not infer peer identity from memid. Match peers by exporter DCNA/DEID.
7. The transport cannot infer NC vs CC from sysfs; users classify regions with
   `UCX_OBMM_NC_MEMIDS` and `UCX_OBMM_CC_MEMIDS`. `UCX_OBMM_CC_MEMIDS` may be
   provided alone for same-node-only `obmm_cc`. Explicit NC/CC lists are
   discovered together when both are configured, then classified, so CC
   reachability does not require a remote CC import when another import already
   provides local exporter identity.

## Mapping Rules

- NC mappings use `open(..., O_RDWR | O_SYNC)` plus
  `mmap(..., MAP_SHARED, PROT_READ | PROT_WRITE, ...)`.
- NC short/control data is visible cross-host without ownership transitions.
- Same-node CC mappings intentionally omit `O_SYNC` so cacheable shared memory
  remains cacheable.
- Cacheable OBMM mappings require `obmm_set_ownership()` transitions for legal
  cross-host access, but cross-host CC is not part of the current transport.
- On arm64 NC mappings, shared control-word atomic RMW must use explicit LSE
  instructions. Do not rely on compiler-default LL/SC atomics or generic
  `ucs_atomic_*`.

## Current Geometry

```text
slot_count      = 96
FIFO_SIZE       = 64
FIFO_ELEM_SIZE  = 520128
BCOPY_SEG_SIZE  = 4096
required_nc     = 3,220,846,912 bytes = 3071.639 MiB
max_short       = 520112 total AM bytes
max_bcopy       = 4096 bytes
wire_format     = UCT_OBMM_WIRE_FORMAT_INLINE32 with iface plane field
short_lanes     = 0
```

Dedicated SPSC short lanes have been removed. `short_lane_count` remains on
the wire as 0 to reject stale lane-based peers.

Prefer 64-byte-aligned `FIFO_ELEM_SIZE` and `BCOPY_SEG_SIZE` unless target
measurements prove otherwise.

## Libobmm API Reference

The public libobmm APIs include:

```c
int    obmm_export(unsigned long len, unsigned long flags, mem_id *id);
int    obmm_unexport(mem_id id, unsigned long flags);
int    obmm_preimport(int device_id, mem_id id, void **mem_addr);
int    obmm_unpreimport(void *mem_addr);
void*  obmm_import(mem_id id, unsigned long flags, int base_node,
                   int base_dist, int *numa);
int    obmm_unimport(mem_id id, unsigned long flags);
int    obmm_set_ownership(int fd, void *start, void *end, int prot);
int    obmm_query_memid_by_pa(unsigned long pa, mem_id *id,
                              unsigned long *offset);
int    obmm_query_pa_by_memid(mem_id id, unsigned long offset,
                              unsigned long *pa);
```

These APIs are context only for the UCT transport. Do not add calls to them
inside `ucx/src/uct/obmm/` without explicit user approval and a new design.

## Diagnose Before Changing

- Error string: grep the exact string and read the emit site.
- Hang: inspect progress, pending, FIFO backpressure, owner bits, and pool
  metadata before changing fences or wire format.
- Performance regression: confirm the actual MPI -> PML UCX -> UCP -> UCT path
  and whether UCP protocol selection matches expectations.
- Logs requested from the user must be short and grep-friendly. Ask for only
  one to three lines or fields when hardware logs must be typed manually.

## Ask Before Assuming

Ask the user before assuming undocumented hardware behavior, NC memory
semantics, future cacheable ownership semantics, libobmm API changes, OMPI
changes, or any broad new capability such as zcopy, RMA, atomics, or PUT/GET.
