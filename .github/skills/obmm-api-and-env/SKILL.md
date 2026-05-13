---
name: obmm-api-and-env
description: >
  Authoritative facts about libobmm, OBMM memory modes, and the target
  environment for the UCX UCT obmm transport. Use whenever writing or reviewing
  code that touches memory setup, mmap, ownership, addressing, or reachability.
---

# OBMM API and Environment Facts

This skill is the source of truth for facts the agent may assume about OBMM.
If a needed fact is not written here, ask the user instead of inventing it.

## libobmm APIs relevant to the transport

From libobmm public headers/docs:

```c
mem_id obmm_export(...);
int    obmm_unexport(mem_id id, unsigned long flags);
int    obmm_preimport(...);
int    obmm_unpreimport(...);
mem_id obmm_import(...);
int    obmm_unimport(mem_id id, unsigned long flags);
int    obmm_set_ownership(int fd, void *start, void *end, int prot);
int    obmm_query_memid_by_pa(unsigned long pa, mem_id *id,
                              unsigned long *offset);
int    obmm_query_pa_by_memid(mem_id id, unsigned long offset,
                              unsigned long *pa);
```

Transport rules:

- UCT must not call export/import/preimport/unexport/unimport lifecycle APIs.
  Production setup performs those before UCX starts.
- `obmm_set_ownership()` is forbidden on NC mappings and allowed only for V3
  hybrid CC chunks.
- `obmm_query_*` APIs are for debug/fault handling, not performance-sensitive
  data paths.

## Current topology facts

V2 NC baseline:

- The working V2 path uses non-cacheable OBMM memory for FIFO/control and
  bcopy descriptors.
- Each node has pre-created OBMM exports/imports before MPI starts.
- UCT discovers shmdevs under `/sys/devices/obmm/obmm_shmdev*/` and maps
  `/dev/obmm_shmdev<memid>`.

V3 hybrid target:

- Each node has one NC export and one CC export.
- For N nodes, each node imports one NC and one CC region from each peer:
  `2 * (N - 1)` imports.
- NC/CC role is not discoverable from sysfs. Users declare it with:
  - `UCX_OBMM_MEM_MODE=nc|hybrid` (default `nc`; pure `cc` rejected)
  - `UCX_OBMM_NC_MEMIDS=<csv>` (mandatory)
  - `UCX_OBMM_CC_MEMIDS=<csv>` (mandatory in hybrid; warned/ignored in nc)
- Users guarantee every node receives the same NC/CC memid lists.
- Each active list has at most one local export and may include multiple
  imports.

## Sysfs/addressing facts

- Root shmdev sysfs fields include `type`, `size`, `allow_mmap`, `priv_len`,
  and `priv`.
- `export_info/` includes export-side fields such as `deid`, `tokenid`, `uba`,
  `node_mem_size`, and `memory_from_user`.
- `import_info/` includes `pa`, `scna`, `dcna`, `deid`, `seid`, `numa_id`, and
  `preimport`.
- Export descriptor `addr` is UBA; import descriptor `addr` / import sysfs PA
  is local physical address. They correspond but are not equal.
- Import memid is local to the importing node and may differ from the peer
  export memid.
- Therefore cross-node reachability must match exporter identity
  `(exporter_dcna, exporter_deid)`, not remote memid.

## Memory mode semantics

### NC mappings

- NC path uses `open(..., O_RDWR | O_SYNC | O_CLOEXEC)` and mmap read/write.
- NC is the only valid mode for FIFO, head/tail, owner bits, pending control,
  and `am_short`.
- Cross-node aligned atomics on NC are a user-confirmed hardware guarantee.
- FIFO reservation must use load + CAS, not FAA, because FAA cannot be rolled
  back on a full FIFO.
- Cross-host ordering uses bus-domain fences:
  `ucs_memory_bus_store_fence()`, `ucs_memory_bus_load_fence()`, and a full bus
  fence before publishing receiver tail after payload loads.

### CC mappings

- CC path opens without `O_SYNC`, mmap's `PROT_NONE`, and uses
  `obmm_set_ownership()` for access.
- User-confirmed V3 ownership granularity: 4 KiB page size.
- CC consistency rule: either all hosts are read/none, or exactly one host is
  writer and all others are none.
- This makes CC unsuitable for FIFO/control without per-message ownership
  handoff. V3 uses CC only for selected `am_bcopy` payload chunks.
- A hybrid TX chunk must be packed under local write ownership, released to
  `PROT_NONE`, and only then advertised by an NC FIFO descriptor.
- A hybrid RX chunk must be acquired `PROT_READ`, synchronously passed to the
  AM callback with flags `0`, released to `PROT_NONE`, and only then may the
  receiver advance the NC FIFO tail.

## What is implemented and what is not

Implemented baseline:

- `am_short`
- `am_bcopy`
- pending arbiter
- cross-node `INTER_NODE` advertisement
- V2 NC paired-desc bcopy
- V3 hybrid design/implementation path for CC bcopy chunks

Not implemented:

- PUT/GET/RMA
- atomics
- AM zcopy
- rkey_ptr over arbitrary peer user memory
- pure CC FIFO/control
- automatic NC/CC detection
- mid-run peer crash recovery for CC chunks

Do not advertise any capability in these non-implemented categories.

## Common traps

- Copying `sm`/`mm` rkey_ptr behavior is wrong: obmm maps only the OBMM shared
  regions, not arbitrary peer process memory.
- Copying mm's CPU-domain fences is wrong for cross-host NC. Use bus-domain
  fences.
- `UCT_IFACE_FLAG_INTER_NODE` is required for cross-host OMPI/UCP address
  packing; without it, OMPI's NET_ONLY address strips obmm.
- Returning `UCS_ERR_BUSY` from `pending_add` as a permanent design causes UCP
  busy-spin under symmetric pressure. Use the arbiter.
- Do not match local imports by remote memid.

## When to ask the user

Ask before assuming:

- new hardware topology
- ownership granularity or mmap offset behavior
- whether a memid list invariant still holds
- cache-coherence or atomic guarantees
- any behavior that would require changing libobmm, OMPI, or UCP outside obmm
