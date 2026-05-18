# Plan — UCT obmm transport: am_short v1

## Problem

Implement `ep_am_short` for the `obmm` UCT transport so UCP / MPI can
send active messages between processes that are connected by the obmm
shared-memory fabric. No other UCT op needs to be implemented in this
pass.

## Confirmed environment facts

(See `.github/skills/obmm-api-and-env/SKILL.md`. Re-stated here for the
plan-review pass; I will fold any new facts back into the skill.)

- Two nodes, each has **already exported one 128 MiB region** to the
  fabric. Same region is used both for self-loopback (intra-node IPC)
  and for cross-node IPC.
- Every process imports both regions (its node's own export + the
  peer node's export). Topology is therefore: per node, 1 export +
  1 import.
- Export / import / preimport / unexport / unimport are done **outside
  UCX**. The UCT transport must NOT call those libobmm APIs.
- Memory is accessed via the character device
  `/dev/obmm_shmdev${memid}`: `open` + `mmap(MAP_SHARED)`. The mapping
  is byte-addressable from user space.
- A region's role (export vs import) is discovered from
  `/sys/devices/obmm/obmm_shmdev${memid}/` by checking whether the
  `export_info/` or `import_info/` subdirectory is present.
- No real obmm hardware in the dev workspace; verification limited to
  build + `ucx_info -d -t obmm` + `ucx_info -c | grep OBMM` + symbol
  inspection.

## Reference patterns (from vector-DB retrieval)

- `ucx/src/uct/sm/mm/base/mm_ep.c::uct_mm_ep_am_short` — sender side
  reservation + inline write + owner-bit publish.
- `ucx/src/uct/sm/mm/base/mm_iface.c::uct_mm_iface_progress` — receiver
  side FIFO poll + `uct_iface_invoke_am`.
- `ucx/src/uct/sm/mm/base/mm_iface.h` — FIFO ctl layout, owner-bit /
  wraparound trick, `uct_mm_fifo_element_t`.
- `ucx/src/uct/sm/self/` — minimal class wiring reference.
- `ucx/src/uct/sm/base/sm_iface.{c,h}` — `uct_sm_iface_t` superclass
  already used by obmm.

## Design

### 1. Discovery (md / iface init)

Scan `/sys/devices/obmm/` once at MD open:

- Each `obmm_shmdev${id}` entry → record `(memid, size_bytes)`.
- If `export_info/` exists → it is **the local node's export region**.
- If `import_info/` exists → it is a **region imported into this node**.
  Record `import_info/dcna` so we can later identify which import maps
  to which remote node.

For v1 (1 local export + 1 remote import per node):
- pick the single `export` shmdev as `local_memid`,
- pick the single `import` shmdev as `peer_memid`.
- If the counts do not match (0 or >1), log + fail md_open with a clear
  error pointing at sysfs.

### 1b. Mapping (NC)

`open("/dev/obmm_shmdev%lu", O_RDWR | O_SYNC)` then
`mmap(NULL, 128 MiB, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)` for both
the local export and the peer import devices. Result is non-cacheable;
no `obmm_set_ownership` calls are needed and the cacheable consistency
model does not apply. User has guaranteed that atomic RMW on NC across
nodes works.

### 2. Memory layout inside the 128 MiB region

The region is a shared FIFO pool, with this layout at offset 0:

```
+-----------------------------------------------------------+
| obmm_pool_hdr_t  (cache-line aligned)                     |   header
|   state              (UNINIT=0 / INITING=1 / READY=2)     |
|   version                                                 |
|   slot_size                                               |
|   slot_count                                              |
|   alloc_bitmap[]      (1 bit per slot, atomic)            |
|   slot_meta[slot_count]:                                  |
|       owner_pid, owner_starttime, generation, state       |
+-----------------------------------------------------------+
| slot[0]  : uct_obmm_fifo_ctl_t + fifo_elems[fifo_size]    |
| slot[1]  : ...                                            |
| ...                                                       |
+-----------------------------------------------------------+
```

**Two-phase init** (fix BLOCKER #1):

1. CAS `state` from `UNINIT` → `INITING`. Winner populates `version`,
   `slot_size`, `slot_count`, zeroes bitmap and slot_meta.
2. Winner does `ucs_memory_bus_store_fence()`, then stores
   `state = READY`.
3. Losers spin on `state == READY` (with timeout), then
   `ucs_memory_bus_load_fence()`, then read the rest. Validate
   `version` and geometry match this build's expectations; mismatch =
   fail md_open.

**Slot generation/quarantine** (fix BLOCKER #3):

- Each `slot_meta[i]` carries `generation` (monotonic u32) plus
  `owner_pid`, `owner_starttime` (from `/proc/<pid>/stat starttime`),
  `state` (`FREE / IN_USE / DEAD`).
- `iface_addr` carries the slot's `generation`, and the slot header
  carries it too. Sender stamps each FIFO write with the expected
  generation; receiver discards elems whose generation doesn't match
  its current one. (Cheap: 4 bytes in elem header.)
- iface destroy:
  1. mark `slot_meta[i].state = DEAD`, bus fence,
  2. bump `generation`,
  3. then clear bitmap bit. Senders that observe DEAD or generation
     mismatch drop the message.

**Bitmap leak / crash recovery** (fix IMPORTANT #4):

- Allocator scans bitmap; for any set bit, if `owner_pid` is gone or
  `/proc/<pid>/stat starttime` no longer matches `owner_starttime`,
  atomically: bump `generation`, set `state=FREE`, clear bit.
- PID-alone is insufficient (PID reuse). Always validate starttime.

Single fixed `slot_size` (config knob, default 128 KiB) so we never
need a real allocator — only a bitmap + meta array.

### 3. iface address

```
typedef struct {
    uint64_t exporter_dcna;  /* exporter identity from sysfs           */
    uint32_t exporter_deid;  /* exporter device id                     */
    uint64_t memid;          /* memid of THIS iface's region           */
    uint32_t slot_index;     /* slot within that region                */
    uint32_t generation;     /* slot generation at iface-create time   */
    uint32_t pid;            /* diagnostics only                       */
} uct_obmm_iface_addr_t;
```

Reachability (fix IMPORTANT #5): `iface_is_reachable_v2` accepts a peer
iff we hold a mapping for `(exporter_dcna, exporter_deid, memid)` —
either as our local export OR as one of our imports. Memid alone is
unsafe (collisions across unrelated obmm fabrics on same host). Drop
`uct_sm_iface_is_reachable`.

(Today `uct_obmm_iface_addr_t` is `uint64_t`; widen it.)

### 4. ep create

`uct_obmm_ep_t` carries:
- `void *peer_fifo_ctl`  — derived from MD-held mapping table keyed by
  `(exporter_dcna, exporter_deid, memid)` from `iface_addr`. No per-ep
  mmap.
- `void *peer_fifo_elems`
- `uint64_t cached_tail`
- `uint32_t expected_generation` — sender stamps this in each elem;
  receiver compares.

The mmap of each region is held once at md level (refcounted across
ifaces).

### 5. am_short send (mirrors mm_ep)

```c
ucs_status_t uct_obmm_ep_am_short(uct_ep_h tl_ep, uint8_t id,
                                  uint64_t header,
                                  const void *payload, unsigned length)
{
    UCT_CHECK_AM_ID(id);
    UCT_CHECK_LENGTH(length + sizeof(header), 0,
                     iface->config.fifo_elem_size -
                         sizeof(uct_obmm_fifo_element_t),
                     "am_short");

    1. atomic FAA on peer_fifo_ctl->head -> our reserved seq
       if (head - cached_tail >= fifo_size) refresh cached_tail
       if still full: return UCS_ERR_NO_RESOURCE
    2. elem = peer_fifo_elems[head & mask]
       elem->generation = ep->expected_generation;
       memcpy header then payload into elem+1
       elem->am_id  = id;
       elem->length = length + sizeof(header);
    3. ucs_memory_bus_store_fence();   /* NC bus-domain release */
    4. set elem->flags owner bit (flips on each wraparound)
    5. UCT_TL_EP_STAT_OP(AM, SHORT, ...)
    6. return UCS_OK
}
```

Pending queue / arbiter is **not** implemented in v1. On `NO_RESOURCE`,
UCP will retry. `ep_pending_add` stays unsupported; `ep_pending_purge`
stays empty.

### 6. iface_progress (receive side)

```c
unsigned uct_obmm_iface_progress(uct_iface_h tl_iface)
{
    elem = local_fifo_elems[tail & mask];
    flags = elem->flags;
    if ((flags & OWNER_BIT) != expected_owner_for_this_pass) return 0;
    ucs_memory_bus_load_fence();   /* NC bus-domain acquire */
    if (elem->generation != iface->generation) {
        /* stale slot reuse / DEAD ep — drop silently */
        tail++;
        return 1;
    }
    invoke_am(iface, elem->am_id, elem+1, elem->length, 0);
    tail++;
    return 1;
}
```

`iface_progress_enable` / `_disable` MUST call
`uct_base_iface_progress_enable(_cb)` / `_disable` so the iface's
`progress` callback is registered with the worker. (Fix BLOCKER #8 —
today they are `ucs_empty_function`.)

### 7. iface_query updates

- `cap.flags |= UCT_IFACE_FLAG_AM_SHORT`
- `cap.am.max_short = fifo_elem_size - sizeof(uct_obmm_fifo_element_t)`
  (TOTAL size including the 8B header — UCT contract per
  `uct/api/uct.h`. Fix IMPORTANT #7.)
- Validate `fifo_elem_size > sizeof(uct_obmm_fifo_element_t)` at iface
  init; otherwise do not advertise `AM_SHORT`.
- Drop `UCT_IFACE_FLAG_EP_CHECK` from caps in v1 (we have no real
  cross-node liveness mechanism — IMPORTANT #9).
- bandwidth / latency: keep current sm-derived defaults; ASK before
  tuning.

### 8. Cross-node memory ordering — use BUS fences

(Fix BLOCKER #2.) `ucs_memory_cpu_*_fence()` is CPU-domain only:
compiler fence on x86, `dmb ish` on arm64 — neither covers NC fabric
peers on another host. Use:

- `ucs_memory_bus_store_fence()` before publishing owner bit / READY
  state (maps to `sfence` on x86, `dmb oshst` on arm64).
- `ucs_memory_bus_load_fence()` after observing owner bit / READY
  (maps to `lfence` on x86, `dmb oshld` on arm64).

The mm transport uses CPU fences because mm peers are same-host and
therefore in the same inner-shareable cache domain. obmm peers are
cross-host — outer-shareable / device-domain barriers are required.

### 9. Build / config wiring

- No new `configure.m4` is needed; v1 does NOT link libobmm — sysfs +
  open + mmap is enough.
- Add no new public headers; obmm transport stays self-contained under
  `ucx/src/uct/obmm/base/`.
- iface cleanup MUST: disable progress (mirroring
  `uct_mm_iface_t_cleanup`), unmap region refs, free slot via
  destroy-protocol above. Skeleton's empty cleanup is a latent
  use-after-free once init is real.

## Resolved decisions (from plan-review with user)

1. **Mapping mode**: data path uses **NC mapping** (`open(... O_SYNC)`
   then mmap). The transport NEVER calls `obmm_set_ownership` and is
   not bound by the OBMM cacheable consistency model.
2. **Cross-node atomics on NC**: user/hardware GUARANTEES that atomic
   RMW (FAA, CAS) on NC mappings works correctly across nodes. This
   lets us mirror mm's multi-producer FIFO design (FAA on head) without
   redesigning to per-ep SPSC.
3. **Pool layout ownership**: UCT fully owns the layout inside the
   128 MiB region. Platform export guarantees the region is
   zero-filled, so CAS-on-magic init is safe.
4. **FIFO defaults**: `OBMM_FIFO_ELEM_SIZE = 2 KiB`, `OBMM_FIFO_SIZE = 64`
   → slot ≈ 128 KiB, region holds ≈ 1024 ifaces. All configurable.
5. **Cross-node coherence ordering**: use **bus-domain** fences
   (`ucs_memory_bus_store_fence` / `ucs_memory_bus_load_fence`) — NOT
   the CPU-domain fences mm uses. mm peers share an inner-shareable
   cache domain; obmm peers do not, so cross-host visibility requires
   `sfence`/`lfence` on x86 or `dmb oshst`/`dmb oshld` on arm64.
6. **Reachability**: `iface_is_reachable_v2` matches on
   `(exporter_dcna, exporter_deid, memid)` from sysfs `import_info/`.
   Drop `uct_sm_iface_is_reachable`.
7. **New config fields under `OBMM_` prefix**: OK (`OBMM_FIFO_ELEM_SIZE`,
   `OBMM_FIFO_SIZE`, `OBMM_SEG_SIZE`, `OBMM_FIFO_MAX_POLL` mirroring
   mm).

## Non-goals (v1)

- bcopy / zcopy / put / get / atomic / pending / signaled wakeup
- multi-region per node, NUMA-aware FIFO placement
- obmm_set_ownership flips during data path
- preimport-aware fast paths

## Verification (no hardware)

Per `ucx-build-verify`:
1. `./autogen.sh && ./contrib/configure-devel && make -j && make install`
2. `ucx_info -d -t obmm` → confirm `am_short: <max>` and
   `UCT_IFACE_FLAG_AM_SHORT`.
3. `ucx_info -c | grep OBMM` → confirm new config fields appear.
4. `nm -D libuct.so | grep uct_obmm_ep_am_short` → exists.
5. Document explicitly that runtime correctness on real fabric is a
   hardware-required follow-up.

## Todo breakdown

(Tracked in SQL.)
