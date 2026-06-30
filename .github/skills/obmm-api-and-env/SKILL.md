# OBMM API And Environment

Use this skill before touching obmm memory setup, mmap flags, sysfs discovery,
reachability, ownership assumptions, or hardware/topology facts.

## Current Transport Scope

- Active UCX transport work is in `ucx/src/uct/obmm/`.
- The target transport is AM-only, short-first, and registers one `obmm` TLS.
- `obmm` advertises `AM_SHORT`, `AM_BCOPY`, `PENDING`, `CONNECT_TO_IFACE`,
  `CB_SYNC`, and `INTER_NODE`.
- NC is mandatory. `UCX_OBMM_SAME_NODE_MEMID` optionally adds one cacheable
  local export selected internally for peers advertising the same exporter
  identity; otherwise endpoints use NC.
- The transport does not advertise `AM_ZCOPY`, PUT/GET/RMA, atomics, `EP_CHECK`,
  AM_DUP, or ERRHANDLE_PEER.
- Cross-node cacheable CC as a UCT transport data path was explored and
  rejected on 2026-06-05. Do not implement or tune staged CC zcopy,
  sender-owned CC, receiver-owned CC, or CC batch/epoch paths unless the user
  explicitly opens a new design.

## Environment Facts

1. Each node currently has one externally exported 4 GiB NC region.
2. Each node imports peer NC regions before UCX/MPI starts.
3. The transport discovers regions through
   `/sys/devices/obmm/obmm_shmdev*/{export_info,import_info}` and maps
   `/dev/obmm_shmdev*` directly.
4. UCT must not call `obmm_export`, `obmm_unexport`, `obmm_import`,
   `obmm_unimport`, `obmm_preimport`, or `obmm_unpreimport`.
5. UCT must not call `obmm_set_ownership()`. NC does not need it, same-node CC
   direct AM does not need it, and the cross-node cacheable CC route is
   rejected.
6. Do not infer peer identity from memid. Match peers by exporter DCNA/DEID.
7. `UCX_OBMM_MEMIDS` is required and must include one local export plus
   required imports. `UCX_OBMM_SAME_NODE_MEMID` is optional, accepts exactly
   one memid when set, and that shmdev must be an export.

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
FIFO_SIZE       = 256
FIFO_ELEM_SIZE  = 131200
BCOPY_SEG_SIZE  = 131072
FIFO_MIN_POLL   = 64
FIFO_MAX_POLL   = 128
required_region = 3,224,385,856 bytes = 3075.014 MiB
max_short       = 131184 total AM bytes
max_bcopy       = 131072 bytes
wire_format     = UCT_OBMM_WIRE_FORMAT_SINGLE_TLS
```

Short and bcopy payloads share one FIFO element allocation with overlapping
ranges. Short starts at byte 16 to preserve its measured-fast inline layout;
bcopy starts at byte 64 for aligned large-fragment writes. `BCOPY_SEG_SIZE` is
an advertised cap and must fit after that offset. Dedicated SPSC short lanes
have been removed.

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
