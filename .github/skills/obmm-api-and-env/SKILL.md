---
name: obmm-api-and-env
description: >
  Authoritative facts about the libobmm API and the target test environment
  for the UCT obmm transport. Use whenever writing or reviewing obmm
  transport code that touches memory setup, addressing, peer reachability, or
  validation scope, to avoid hallucinating runtime behavior we do not actually
  have hardware to verify locally.
---

# OBMM API and Test Environment

This skill is the **single source of truth** for what the agent may assume
about libobmm and the deployment topology. If something is not stated here,
the agent must ASK the user instead of inventing it.

## libobmm public API (from `obmm/src/libobmm/libobmm.h`)

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

Key data structures:

- `struct obmm_mem_desc { addr; length; seid[16]; deid[16]; tokenid; scna;
  dcna; priv_len; priv[]; }` — the descriptor returned by export and
  consumed by import.
- `struct obmm_preimport_info { pa; length; base_dist; numa_id; seid; deid;
  scna; dcna; priv_len; priv[]; }`
- `mem_id` is `uint64_t`; `OBMM_INVALID_MEMID == 0`.

## Test environment — current ground rules

These are the stable facts the agent may rely on without re-asking:

1. The export / import / preimport / unimport / unexport lifecycle is
   handled outside the UCX transport — **the obmm UCT transport must NOT
   call** `obmm_export`, `obmm_unexport`, `obmm_import`, `obmm_unimport`,
   `obmm_preimport`, or `obmm_unpreimport` at runtime.
2. UCT discovers pre-created shmdev regions by scanning
   `/sys/devices/obmm/obmm_shmdev*/` and inspecting whether each device has
   `export_info/` or `import_info/`.
3. Region selection is configuration-driven rather than hardcoded. The current
   transport requires `OBMM_CC_MEMIDS` for any usable TL. `OBMM_NC_MEMIDS` is
   needed only when the cross-node `obmm_nc` TL is actually in use: NC backs
   cross-node eager/control, while CC backs same-node `obmm_cc` and
   obmm_nc's CC bulk-window storage.
4. The active eager data path still uses NC mappings via
   `open("/dev/obmm_shmdev${memid}", O_RDWR | O_SYNC)` + `mmap`. Cacheable
   mappings are a separate design space and must not be treated as a drop-in
   replacement for the shared eager FIFO.
5. **No hardware is available** in the development environment. Do not attempt
   to run `mpirun`, real `ucx_perftest`, or any test that requires the obmm
   device. Local validation is limited to:
     - `make` / `make install` succeeding,
     - `ucx_info -d` listing the obmm component, md, and tl,
     - `ucx_info -c` showing OBMM_* env vars,
     - static review against this skill and `uct-transport-patterns`.
6. The current in-tree obmm baseline has already passed the full OSU
   micro-benchmark suite on the real target environment. Treat that as the
   validated correctness baseline for the transport's currently advertised
   AM-only capabilities, but do not describe it as re-validated by the local
   workspace.

## Current validated transport baseline

- The public TLs are `obmm_cc` and `obmm_nc`.
- `obmm_cc` is same-node-only and advertises:
  `AM_SHORT`, `AM_BCOPY`, `PENDING`, `CONNECT_TO_IFACE`, `CB_SYNC`.
- `obmm_nc` is cross-node-only and advertises:
  `AM_SHORT`, `AM_BCOPY`, `PENDING`, `CONNECT_TO_IFACE`, `CB_SYNC`,
  `INTER_NODE`.
- Both TLs share the same packed worker-address format. `obmm_cc` keeps the
  b4-style same-node direct-pack eager ring and currently promotes the
  untouched default CC eager `BCOPY_SEG_SIZE` to `65472` bytes to reduce large
  local-message fragmentation without reintroducing the older receiver-owned
  desc-pool path. `obmm_nc` still uses the internal CC bulk-window path.
- The current baseline does **not** advertise:
  `AM_ZCOPY`, PUT/GET/RMA, atomics, or `EP_CHECK`.
- The current pending path uses `ucs_arbiter_t`; `pending_add` queues rather
  than returning success-shaped no-op stubs.

## Locked-in design decisions (do not change without re-asking)

These were explicitly decided with the project owner during the initial
transport design reviews. They override any conflicting suggestion the
agent may otherwise default to (notably the mm transport's behavior).

1. **NC mapping for the current eager FIFO path.** Open shmdev with
   `O_RDWR | O_SYNC` and mmap with `MAP_SHARED` for the active shared FIFO
   region. This puts the eager path into a non-cacheable mapping, which:
     - bypasses the OBMM cacheable consistency model (writers and
       readers from any host coexist),
     - removes the need to call `obmm_set_ownership` on that path.
2. **Cross-node atomic RMW on NC is guaranteed only through explicit
   arm64 LSE instructions.** The project owner has stated that NC
   mappings support atomic FAA / CAS across nodes, but compiler-default
   LL/SC atomics are unusable on this hardware. The transport must
   therefore use explicit LSE atomics for every shared control-word RMW
   on the NC data path; do not rely on generic `ucs_atomic_*`,
   `__sync*`, or `__atomic*` lowering on aarch64.
3. **UCT owns the in-region layout it manages.** For shared regions whose
   layout belongs to obmm UCT, the transport places its own header
   (state / version / slot bitmap / slot_meta / fixed-size slots) at the
   start of the mapped region. A two-phase init is used:
   `state` transitions UNINIT→INITING (CAS) → fill geometry → bus
   fence → store READY. Losers spin on READY then bus-load fence.
   Single-magic init is racy (geometry not yet visible) and must
   not be used.
4. **Discovery and peer matching are explicit.** Region discovery comes from
   sysfs plus the current memid configuration. Reachability and mapping table
   keys are `(exporter_dcna, exporter_deid, memid)` from sysfs — NOT memid
   alone (collision-prone). If multiple region roles are active, expose or
   validate the role explicitly instead of guessing from memid order.
5. **Self-loopback inside one node** is supported: same-node processes
   communicate by both mapping the local export region (the imported
   "peer region" entry simply will not exist in the single-node case,
   or will equal the local one — handle both).
6. **Slot lifecycle uses generation tokens on slot-based paths.** Each slot has
   `(owner_pid, owner_starttime, generation, state)` in slot_meta.
   `iface_addr` and every FIFO elem carry `generation`; receiver
   discards mismatches. Destroy = mark DEAD → bus fence → bump
   generation → clear bit. Crash recovery: scan bitmap, validate
   `/proc/<pid>/stat starttime`, reclaim. PID alone is insufficient
   (PID reuse).
7. **Cross-node memory ordering uses BUS-domain fences.**
   `ucs_memory_bus_store_fence()` / `ucs_memory_bus_load_fence()`
   (sfence/lfence on x86, `dmb oshst`/`dmb oshld` on arm64). The
   CPU-domain fences mm uses (`ucs_memory_cpu_*_fence`) are
   inner-shareable only and DO NOT cover cross-host NC visibility.

## OBMM consistency model — why NC matters for shared eager FIFO

From `obmm/doc/libobmm.md` and `obmm/doc/obmm_set_ownership.md`:

- For **cacheable** OBMM mappings, at any moment all hosts touching a
  region must be in one of two states:
    a) all hosts are PROT_READ or PROT_NONE, OR
    b) exactly one host has writers; all other hosts must be PROT_NONE.
- A receive-FIFO model (writer on the sending host, reader on the
  receiving host, on the SAME region) violates this: it would require
  one host to write while another host reads simultaneously.
- Therefore the obmm transport CANNOT use cacheable mappings for the
  shared eager FIFO region without expensive ownership flips per message.
- NC (O_SYNC) mappings are exempt: per the doc, "用户无需关心一致性
  模型，所有的用户均具备读写权限". This is why decision (1) above
  is mandatory, not optional.
- This does **not** rule out future CC designs on disjoint regions with an
  explicit ownership protocol; it only rules out treating CC as a transparent
  substitute for the shared eager FIFO.

## Implications for the transport design

These follow from the facts above and should be treated as defaults; ask
the user before deviating:

- **Memory registration** (`uct_md_ops_t::mem_reg` / `mem_dereg`) still does
  not need to call libobmm for the current AM-only transport surface. The
  existing dummy registration hooks are appropriate because current traffic
  uses the pre-imported obmm region rather than arbitrary remote user buffers.
- **Address exchange** is now unified:
  `device_addr` carries the peer process's NC/shared exporter identity, while
  `iface_addr` carries NC eager slot geometry, the CC exporter plus slot
  identity, the NC bulk-control slot identity, and the CC bulk-window layout.
  Keep exporter identity explicit; do not regress back to implicit memid-order
  assumptions.
- **Reachability**: `iface_is_reachable_v2` currently validates exporter
  identity plus wire geometry against the MD's mapped export/import regions.
  It must not regress to same-host-only `uct_sm_iface_is_reachable` logic.
- **Progress wiring**: preserve the current `uct_base_iface_progress_enable`
  / `uct_base_iface_progress_disable` wiring. Regressing these hooks to empty
  stubs would prevent UCP from polling the iface.
- **EP_CHECK**: do NOT advertise `UCT_IFACE_FLAG_EP_CHECK` in the current
  baseline. There is still no cross-node liveness check for this transport.
- **Ownership / `obmm_set_ownership`**: do not add it to the active NC eager
  path. The current unified transport only allows it on the disjoint CC bulk
  window path.
- **Atomic helpers on aarch64 NC mappings**: shared head/state/bitmap
  words must use explicit LSE CAS-based helpers in the obmm transport.
  Current sender-side FIFO reservation uses CAS on `peer_ctl->head`,
  not a token lock and not generic compiler-lowered atomics.

## What to ASK the user before writing code

Do not invent answers to any of these. Use the `ask_user` tool:

1. Whether multiple obmm ifaces per process are expected, or strictly
   one peer per local iface (current assumption: 1 iface per process,
   pool holds many ifaces from many processes).
2. What the wire `am_id` / header / payload alignment requirements are
   on the obmm hardware (e.g. 64 B cache line? 256 B?). Default plan
   aligns elements to 64 B; confirm before tuning.
3. What region-role split and memid assignment the current implementation may
   assume, if the change depends on more than the explicitly configured
   `OBMM_NC_MEMIDS` / `OBMM_CC_MEMIDS`, with CC always required for the current
   transport family and NC required only for `obmm_nc`.

## Forbidden assumptions

- Do NOT assume libobmm performs its own synchronization — am_short
  ordering must be enforced by the transport (release / acquire fences,
  same as mm).
- Do NOT use cacheable mappings on any concurrently read/write eager FIFO
  region. NC (`O_SYNC`) is mandatory for the current shared eager path.
- Do NOT call any `obmm_*` runtime API in the UCT transport unless this
  document explicitly allows it. In particular do NOT add
  `obmm_set_ownership` to the active NC eager path; today it is only allowed
  on the isolated CC bulk data windows.
- Do NOT assume a fixed region count, size, or memid ordering unless the user
  explicitly provides that constraint for the task at hand.
- Do NOT use generic compiler-lowered atomics for NC shared control
  words on aarch64. Compiler-default LL/SC atomics are unusable on this
  hardware; use explicit LSE atomics in the obmm transport helpers.
- Do NOT use `ucs_memory_cpu_*_fence()` on the obmm data path. They
  are inner-shareable / compiler-only and do not cover cross-host NC
  visibility. Use `ucs_memory_bus_store_fence()` /
  `ucs_memory_bus_load_fence()` (sfence/lfence on x86,
  `dmb oshst`/`dmb oshld` on arm64). The mm transport gets away with
  CPU fences only because mm peers share an inner-shareable cache
  domain; obmm peers do not.

## Why cacheable + manual cache management was rejected for eager FIFO

Asked and answered during plan-review. Do not re-litigate without new
information about the obmm fabric.

1. **OBMM cacheable consistency is enforced at the page-table level**,
   not at the cache level. `obmm/doc/libobmm.md:196-211` and
   `obmm/doc/obmm_set_ownership.md` require that at any moment either
   all hosts touching a region are PROT_READ/PROT_NONE, or exactly
   one host has writers and all other hosts are PROT_NONE. Even a
   correctly `clflush`-ed write from host A is not legal to read on
   host B until ownership is flipped via the `obmm_set_ownership()`
   syscall. That is one syscall per message — incompatible with
   `am_short` latency goals.
2. **arm64 user-space lacks `dc ivac`** (invalidate by VA to PoC, EL0
   typically only exposes `dc civac` = clean+invalidate). A reader-side
   "discard cache, then load" sequence would have to clean+invalidate
   on every poll, doubling the writer-side cost.
3. **UCX has no precedent** for cross-host cacheable shared memory with
   manual coherence on a hot data path. `ucs_arch_clear_cache` exists
   (x86: `mfence; clflush; mfence` per line; arm64: `dc cvau` +
   `ic ivau`) but is used only for instruction-cache coherence
   (JIT / code patching), never for transport data. All cross-host
   UCX transports either use a NIC (NIC handles coherence) or rely
   on a HW-coherent fabric. Inventing a new pattern here would carry
   risk far above what NC mapping costs.
4. **Bulk-flip ownership** (writer holds write, batches N messages,
   flips, reader batches N reads) belongs to a different design space:
   it may make sense for dedicated CC local/bulk transfers, but it is not
   a drop-in replacement for the current AM short/bcopy eager baseline.

NC (`O_SYNC`) avoids all four issues at the cost of uncached load/store
performance on the shared eager FIFO. That does not preclude future
ownership-based CC designs on separate regions.
