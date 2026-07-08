# OBMM API And Environment

Use this skill before touching obmm memory setup, mmap flags, sysfs discovery,
reachability, ownership assumptions, or hardware/topology facts.

## Current Transport Scope

- Active UCX transport work is in `ucx/src/uct/obmm/`.
- This skill records OBMM environment and API facts. It should not define the
  current UCT obmm design; use `ucx/src/uct/obmm/DESIGN.md` for transport
  policy, capability, wire-format, and lifecycle decisions.

## Environment Facts

1. The hardware environment can pre-provision multiple NC export/import shmdev
   blocks. In the current prepared test environment, UCX-owned blocks are
   labelled with private metadata `ucx-obmm:NN`.
2. Peer imports may be prepared outside UCX before UCX/MPI starts. Whether UCT
   performs export/import itself is a transport design decision documented in
   `DESIGN.md`, not in this skill.
3. Local controller identity is available from sysfs, for example under
   `/sys/devices/ub_bus_controller0/00001/` via files such as `eid` and
   `primary_cna`.
4. OBMM shmdev metadata is available through
   `/sys/devices/obmm/obmm_shmdev*/{export_info,import_info,priv,priv_len}`.
   The device node is `/dev/obmm_shmdev<memid>`.
5. OBMM allocation granularity in the current environment is 2 MiB.
6. A memid is local to the host/device namespace and is not a stable
   cross-node peer identity. Cross-node identity is carried by exporter/import
   metadata such as CNA/EID and any higher-level private metadata.

## Mapping Rules

- NC mappings use `open(..., O_RDWR | O_SYNC)` plus
  `mmap(..., MAP_SHARED, PROT_READ | PROT_WRITE, ...)`.
- NC short/control data is visible cross-host without ownership transitions.
- Cacheable OBMM mappings require ownership transitions for legal cross-host
  access.
- On arm64 NC mappings, shared control-word atomic RMW must use explicit LSE
  instructions. Do not rely on compiler-default LL/SC atomics or generic
  `ucs_atomic_*`.

## Libobmm API Reference

The public libobmm APIs include:

```c
mem_id obmm_export(const size_t length[OBMM_MAX_LOCAL_NUMA_NODES],
                   unsigned long flags, struct obmm_mem_desc *desc);
int    obmm_unexport(mem_id id, unsigned long flags);
int    obmm_preimport(struct obmm_preimport_info *preimport_info,
                      unsigned long flags);
int    obmm_unpreimport(const struct obmm_preimport_info *preimport_info,
                        unsigned long flags);
mem_id obmm_import(const struct obmm_mem_desc *desc, unsigned long flags,
                   int base_dist, int *numa);
int    obmm_unimport(mem_id id, unsigned long flags);
int    obmm_set_ownership(int fd, void *start, void *end, int prot);
int    obmm_query_memid_by_pa(unsigned long pa, mem_id *id,
                              unsigned long *offset);
int    obmm_query_pa_by_memid(mem_id id, unsigned long offset,
                              unsigned long *pa);
```

These APIs are context for transport work. Whether the UCT transport calls
them is governed by `ucx/src/uct/obmm/DESIGN.md`.

## Assumption Boundary

If a needed OBMM API, sysfs, mmap, cacheability, ownership, or topology fact is
not listed here or confirmed in source, ask the user before relying on it.
