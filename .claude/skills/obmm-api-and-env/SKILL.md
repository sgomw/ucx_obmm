---
name: obmm-api-and-env
description: >
  Authoritative facts about the libobmm API, the OBMM cacheable (CC)
  consistency model, and the target test environment.  NC is deprecated;
  the CC model is the primary design constraint for the transport redesign.
  Use whenever writing or reviewing obmm transport code that touches memory
  setup, ownership, addressing, or reachability.
---

# OBMM API, Consistency Model, and Test Environment

This skill is the **single source of truth** for what the agent may assume
about libobmm, the OBMM consistency model, and the deployment topology.
If something is not stated here, ASK the user instead of inventing it.

## ⚠️  NC deprecation

Non-cacheable (NC / `O_SYNC`) mappings are being **removed** from the next
OBMM hardware revision.  The current in-tree NC transport is validated legacy
and must be replaced with a **CC-only (cacheable) redesign**.

## OBMM cacheable consistency model — PRIMARY CONSTRAINT

From `obmm/doc/libobmm.md` and `obmm/doc/obmm_set_ownership.md`:

> For **cacheable** OBMM mappings, at any moment all hosts touching a region
> must be in one of two states:
>   a) all hosts are PROT_READ or PROT_NONE, OR
>   b) exactly one host has PROT_WRITE; **all other hosts must be PROT_NONE**.

Key implications:

- A **receive-FIFO pattern** (host A writes into host B's receive buffer while
  host B reads it) is **illegal** — it requires simultaneous PROT_WRITE on A's
  mapping and PROT_READ on B's mapping of the same region.
- Every cross-host data transfer is an **ownership handover**: writer acquires
  → writes → releases → reader acquires → reads → releases.
- Permission changes use `obmm_set_ownership(fd, start, end, prot)` or
  dynamic `mmap`/`munmap`.
- `set_ownership` operates at page granularity (start/end must be PAGE_SIZE
  aligned).  The consistency model is enforced per-region — when any host
  holds PROT_WRITE on any page, ALL other hosts must be PROT_NONE on the
  entire region.

## libobmm public API

```c
mem_id obmm_export(const size_t length[OBMM_MAX_LOCAL_NUMA_NODES],
                   unsigned long flags, struct obmm_mem_desc *desc);
int    obmm_unexport(mem_id id, unsigned long flags);

int    obmm_preimport(struct obmm_preimport_info *preimport_info,
                      unsigned long flags);
int    obmm_unpreimport(const struct obmm_preimport_info *preimport_info,
                        unsigned long flags);

mem_id obmm_export_useraddr(int pid, void *va, size_t length,
                            unsigned long flags, struct obmm_mem_desc *desc);

mem_id obmm_import(const struct obmm_mem_desc *desc, unsigned long flags,
                   int base_dist, int *numa);
int    obmm_unimport(mem_id id, unsigned long flags);

int    obmm_set_ownership(int fd, void *start, void *end, int prot);

int    obmm_query_memid_by_pa(unsigned long pa, mem_id *id,
                              unsigned long *offset);
int    obmm_query_pa_by_memid(mem_id id, unsigned long offset,
                              unsigned long *pa);
```

Key types:
- `struct obmm_mem_desc { addr; length; seid[16]; deid[16]; tokenid; scna; dcna; priv_len; priv[]; }`
- `struct obmm_preimport_info { pa; length; base_dist; numa_id; seid; deid; scna; dcna; priv_len; priv[]; }`
- `mem_id` is `uint64_t`; `OBMM_INVALID_MEMID == 0`.

## `obmm_set_ownership` semantics

```c
int obmm_set_ownership(int fd, void *start, void *end, int prot);
```

- `fd` — open file descriptor for the shmdev device.
- `start`, `end` — page-aligned virtual address range within the mmap'd region.
- `prot` — target protection: `PROT_NONE`, `PROT_READ`, or `PROT_WRITE` (implies read).
- Returns 0 on success, -1 with errno on failure.
- **Cannot be used on NC mappings** (returns `EINVAL`).
- Key errors: `EINVAL` (invalid prot or NC mapping), `EBUSY` (max readers/writers
  exceeded), `EFAULT` (VMA not found / device mismatch), `ENOTRECOVERABLE`
  (cache flush failure).

## Test environment — IMMUTABLE FACTS

1. **Two nodes**, node 0 and node 1.
2. Each node has **already exported a 3072 MiB CC memory region** to the other side,
   before any UCX / MPI process starts.
3. Each node has **already imported** the peer's 3072 MiB CC region.
4. The export / import / preimport lifecycle is **handled outside UCX**.
   The UCT transport must NOT call `obmm_export`, `obmm_unexport`, `obmm_import`,
   `obmm_unimport`, `obmm_preimport`, or `obmm_unpreimport` at runtime.
5. Access to local and peer regions: `open("/dev/obmm_shmdev${memid}", ...)`
   + `mmap`.  Discovery via `/sys/devices/obmm/obmm_shmdev*/`.
6. **No hardware or Linux toolchain in the development environment.**  Do not
   run `mpirun` or `ucx_perftest` locally.  Local validation is limited to
   static review.  Real validation: two-node Linux setup with MPI + OSU.
7. **CC mapping**: for cacheable mappings, open with `O_RDWR` (no `O_SYNC`).

## Topology and addressing

- Per node: exactly 1 export region and 1 import region.
- Discovery: `/sys/devices/obmm/obmm_shmdev*/{export_info,import_info}`.
- Reachability key: `(exporter_dcna, exporter_deid, memid)` — NOT memid alone.
- Self-loopback: same-node processes both map the local export region.

## Forbidden (legacy NC rules that do NOT apply to CC)

These rules were specific to the NC transport and are **not constraints on
the CC redesign** (unless explicitly re-confirmed):

- ~~NC mapping (`O_RDWR | O_SYNC`) is mandatory~~ — CC uses `O_RDWR` without `O_SYNC`.
- ~~Do not call `obmm_set_ownership`~~ — for CC, `obmm_set_ownership` IS the
  permission management tool (alongside `mmap`/`munmap`).
- ~~Use bus-domain fences (`dmb oshst`/`dmb oshld`)~~ — CC memory uses standard
  CPU cache coherence.  `dmb osh` may still be needed for store ordering before
  releasing ownership, but the NC fence discipline does not apply.
- ~~Use explicit LSE atomics on aarch64~~ — CC memory supports standard atomics.
  Whether LSE is still preferred for performance is a measurement question, not
  a correctness requirement.
- ~~Two-phase pool init with CAS on NC control words~~ — CC pool init (if a pool
  is needed at all) can use regular atomics on cacheable memory.

## What to ASK the user before writing code

1. CC atomics: are standard `ucs_atomic_*` sufficient, or should LSE still be
   preferred for cross-host CAS on cacheable memory?
2. Fences: what fence (if any) is needed after a store and before
   `obmm_set_ownership(PROT_NONE)` / `munmap` to ensure the peer sees the
   data? Is `dmb ish` sufficient or is `dmb osh` still required?
3. Multi-connection design: can multiple UCT ifaces/eps share the same region
   under the CC model?  How is the region partitioned?
4. What is the expected process count per node?  Current design is slot_count=32;
   does CC design need the same scale?
