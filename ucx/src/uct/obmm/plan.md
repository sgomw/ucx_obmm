# Session Plan: FIFO-Only AM Path

## Trigger

The 96-slot / 192-lane SPSC short-lane build hung silently in three
100-process `osu_multi_lat` runs, at message sizes 1024, 4, and 16. The
working hypothesis is that the deterministic lane design has a correctness
hole under high process counts. This session removes that lane path and routes
`am_short` through the shared NC FIFO as well.

## Current Geometry Decision

- `UCT_OBMM_POOL_SLOT_COUNT = 96`
- `UCT_OBMM_SHORT_LANE_COUNT = 0`
- `FIFO_SIZE = 128`
- `FIFO_ELEM_SIZE = 2048`
- `BCOPY_SEG_SIZE = 19776`

The shared FIFO is now the only AM publication path:

```
flags & BCOPY    -> payload lives in desc[idx]
!(flags & BCOPY) -> FIFO element carries inline am_short [header|payload]
```

`short_lane_count` remains in `uct_obmm_iface_addr_t` and is set to 0, so
wireup rejects peers from the prior lane-based layout.

## Size Check

With the current C layout on the target 64-byte cacheline architectures:

```
pool_overhead = 2368 bytes
slot_stride   = 128 + 128 * (2048 + 19776)
              = 2793600 bytes
required      = 2368 + 96 * 2793600
              = 268187968 bytes
              = 255.764 MiB
```

This fits in a 256 MiB region with about 0.236 MiB of headroom.

## Self-Review Checklist

- Do not change advertised UCT capabilities.
- Keep `am_short` and `am_bcopy` using the same FIFO reservation and pending
  backpressure path.
- Keep `BCOPY` as the wire discriminator; absence of `BCOPY` means inline
  FIFO short.
- Keep full bus-fence tail release after invoking AM handlers.
- Carry `slot_count` and `short_lane_count` in `uct_obmm_iface_addr_t` so
  reachability rejects stale peers before ep creation.
- Update workflow docs that still describe SPSC short lanes.
- Local validation is static only in this Windows workspace; Linux build and
  hardware validation remain target-side work.
# Session Plan: CC-Accelerated Large-Message am_bcopy

## Trigger

Current NC AM-only transport passes OSU but has untapped bandwidth headroom
for large messages.  The deployment has per-node CC (cache-coherent) regions
alongside the NC region.  CC bandwidth substantially exceeds NC and even
kernel posix bandwidth.

## Environment Facts (user-confirmed)

- Per node: 2 export regions (1 NC 256 MiB + 1 CC 3 GiB), 2 import regions.
- CC regions are shmdev-accessible (`allow_mmap=1`), created with
  `OBMM_EXPORT_FLAG_ALLOW_MMAP` / `OBMM_IMPORT_FLAG_ALLOW_MMAP`.
- CC access: `open(O_RDWR)` (no O_SYNC) + `mmap(PROT_NONE)`, ownership
  managed by `obmm_set_ownership(fd, start, end, prot)`.
- Ownership flip cost ~50 us per call (2 on sender, 2 on receiver = 4 per
  CC message).  The CC bandwidth gain must overcome this overhead; the user
  will measure to find the crossover threshold.
- Ownership granularity: up to PAGE_SIZE (4K), but PMD_SIZE (2M) is the
  natural efficient unit.

## CC Consistency Model Recap

From `obmm_set_ownership.md`:

- CC is mapped via `open(fd, O_RDWR)` + `mmap(PROT_NONE)`.  Start with no
  access, acquire via `obmm_set_ownership`.
- At any moment: either all hosts are READ/NONE, or exactly one host has
  WRITE and all others are NONE.
- When the last writer releases → hardware cache writeback.
- When the last reader releases → hardware cache invalidation.
- `obmm_set_ownership` works on page-aligned ranges.
- **Constraint**: the sender writes into its LOCAL CC EXPORT region;
  the receiver reads from its CC IMPORT of that same region.  This is the
  only viable data-flow for a FIFO-like pattern.

## Design

### 1. Configuration Split

Replace `OBMM_MEMIDS` with:

| Knob | Default | Meaning |
|------|---------|---------|
| `OBMM_NC_MEMIDS` | `""` | NC shmdev allow-list (was OBMM_MEMIDS) |
| `OBMM_CC_MEMIDS` | `""` | CC shmdev allow-list |
| `OBMM_CC_ENABLE` | `0` | 1 = route large am_bcopy via CC |
| `OBMM_CC_THRESH` | `32768` | min bytes to use CC path |
| `OBMM_CC_BUF_SIZE` | `2097152` | CC buffer size, PMD_SIZE-aligned (2M) |

### 2. Region Management

`uct_obmm_md_t` gains two parallel region arrays:
- `nc_regions[]` / `nc_num_regions` / `nc_export_idx` (existing logic)
- `cc_regions[]` / `cc_num_regions` / `cc_export_idx` (new)

NC regions open with `O_RDWR | O_SYNC` as before.
CC regions open with `O_RDWR` (cacheable), mapped `PROT_NONE` initially.

`uct_obmm_md_find_cc_import_region()` — locates peer CC import by exporter
identity, symmetric to the existing NC import lookup.

### 3. CC Buffer Pool

The CC export region is divided into fixed-size PMD_SIZE-aligned buffers.
One NC FIFO slot = one CC buffer, 1:1.  CC buffer index = NC FIFO index
modulo `cc_num_bufs`.  This avoids a separate allocator: CC buffer
lifecycle is naturally gated by the NC FIFO tail — the sender can only
reuse CC buffer `N` once the receiver has released NC FIFO slot `N`.

`cc_num_bufs = min(cc_export_size / cc_buf_size, fifo_size)`.

CC buffer pool metadata (a `cc_buf_gen[]` per-buffer generation array,
lives in NC region to avoid CC ownership issues) is NOT needed under the
1:1 coupling: the NC FIFO element's OWNER bit and generation already
prevent stale-read hazards.  When the ring wraps, the OWNER bit inverts
and the receiver naturally skips it.

### 4. Wire Format — New FLAG_CC

`UCS_BIT(2)` (value 0x04) added to `uct_obmm_fifo_element_t.flags`.

When FLAG_CC is set:
- BCOPY is NOT set (mutually exclusive).
- `elem->header` carries `cc_buf_offset` — the offset within the sender's
  CC export region (uint64_t, fits in 8 bytes).  The receiver adds this
  to its CC import base to locate the payload.
- `elem->length` is the payload size.
- The NC `desc[N]` area is NOT used for this element.

### 5. Iface Address — New CC Geometry Fields

`uct_obmm_iface_addr_t` grows two fields:

| Field | Size | Meaning |
|-------|------|---------|
| `cc_enabled` | uint32_t | non-zero if CC path is active for this iface |
| `cc_buf_size` | uint32_t | CC buffer size in bytes |

Reachability check validates CC geometry symmetry (both sides must agree
on `cc_enabled` and `cc_buf_size`).

### 6. Iface State

`uct_obmm_iface_t` gains:

```c
uct_obmm_region_t *cc_region;       /* local CC export */
int                cc_export_fd;     /* cached fd for obmm_set_ownership */
uint32_t           cc_enabled;
uint32_t           cc_buf_size;
uint32_t           cc_num_bufs;
uint32_t           cc_thresh;        /* threshold from config */
```

### 7. EP State

`uct_obmm_ep_t` gains:

```c
uct_obmm_region_t *cc_peer_region;  /* peer CC import mapping */
int                cc_peer_fd;       /* fd for obmm_set_ownership on peer */
uint32_t           cc_enabled;
uint32_t           cc_buf_size;
```

### 8. Sender Flow (am_bcopy, length >= cc_thresh)

```
1. Reserve NC FIFO slot (existing CAS flow).
2. cc_idx = (head & fifo_mask) % cc_num_bufs.
3. cc_offset = cc_idx * cc_buf_size.
4. cc_ptr = cc_region->base + cc_offset.
5. obmm_set_ownership(cc_export_fd, cc_ptr, cc_ptr + cc_buf_size,
                       PROT_READ | PROT_WRITE).
6. length = pack_cb(cc_ptr, arg).
7. obmm_set_ownership(cc_export_fd, cc_ptr, cc_ptr + cc_buf_size,
                       PROT_NONE).
8. Fill NC FIFO elem: am_id, length, generation, header=cc_offset.
9. Bus store fence.
10. elem->flags = OWNER_BIT | FLAG_CC.
```

### 9. Receiver Flow (iface_progress, FLAG_CC)

```
1. Observe elem->flags & OWNER matches, elem->flags & FLAG_CC set.
2. cc_offset = elem->header.
3. cc_ptr = cc_peer_region->base + cc_offset.
4. obmm_set_ownership(cc_peer_fd, cc_ptr, cc_ptr + elem->length,
                       PROT_READ).
5. uct_iface_invoke_am(iface, am_id, cc_ptr, length, 0).
6. obmm_set_ownership(cc_peer_fd, cc_ptr, cc_ptr + elem->length,
                       PROT_NONE).
7. (Existing tail bump + full bus fence after all elements.)
```

### 10. Error Handling

If any `obmm_set_ownership` call fails:
- On sender: return UCS_ERR_NO_RESOURCE; UCP retries via pending.
- On receiver: log error, skip element, bump tail.  (The CC buffer data
  is lost for this element; UCP handles retransmission.)

All CC-path decisions use a `UCT_CHECK_PARAM`-style assertion that the
CC `info.allow_mmap` is true before using the fd for ownership calls.

## Implementation Task List

1. `obmm_fifo.h` — add `FLAG_CC` (UCS_BIT(2))
2. `obmm_sysfs.h` — update comment (no structural change; CC discovery
   reuses the same sysfs path)
3. `obmm_region.h/c` — add `uct_obmm_region_open_cc()` that opens without
   O_SYNC and mmap's PROT_NONE
4. `obmm_md.h/c` — split regions into NC and CC arrays; add
   `NC_MEMIDS`/`CC_MEMIDS` config; add CC import lookup
5. `obmm_iface.h/c` — CC fields, CC config knobs, CC-pool attach,
   updated iface_addr and reachability, CC path in progress
6. `obmm_ep.h/c` — CC peer region ptr, CC-threshold routing in am_bcopy
7. `DESIGN.md` — update wire format and capabilities
8. `Makefile.am` — add new source files if any

## Files NOT Changed

- `obmm_pool.h/c` — NC pool logic unchanged
- `obmm_atomic.h` — no new atomics needed
- `ompi/`, `obmm/` — read-only per AGENTS.md
