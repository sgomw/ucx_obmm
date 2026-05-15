# obmm UCT transport design notes

This file is the source of truth for the on-region wire format and data-path
semantics of the `obmm` UCT transport. Update it before changing layout,
capabilities, or synchronization rules.

Status:

- v2: stable NC implementation with `am_short`, `am_bcopy`, pending arbiter,
  and paired NC bcopy descriptors.
- v3: hybrid mode. Preserve the v2 NC path, and use cacheable (CC) memory only
  for selected `am_bcopy` payloads.
- MD local host allocation is supported for UCT framework/perftest send/receive
  buffers. This is heap-backed local memory only; obmm still does not implement
  memory registration, rkeys, rkey_ptr, RMA, or zcopy.

## Environment facts and hard constraints

- Export/import/preimport are done outside UCX. UCT must not call
  `obmm_export`, `obmm_unexport`, `obmm_import`, `obmm_unimport`,
  `obmm_preimport`, or `obmm_unpreimport`.
- NC/CC role is not discoverable from sysfs. Users must classify memids with
  `UCX_OBMM_NC_MEMIDS` and, in hybrid mode, `UCX_OBMM_CC_MEMIDS`.
- Import memid is local to the importing node; do not match peers by memid.
  Cross-node matching uses exporter identity `(exporter_dcna, exporter_deid)`.
- NC mappings are opened with `O_SYNC` and mmap'd read/write.
- CC mappings are opened without `O_SYNC`, mmap'd `PROT_NONE`, and accessed
  only through `obmm_set_ownership()` transitions. Local CC exports are opened
  read/write for TX chunks; peer CC imports are opened read-only because RX only
  takes `PROT_READ` ownership.
- `obmm_set_ownership()` is valid only for CC mappings. V3 hardware ownership
  granularity is 4 KiB page size.
- CC consistency permits either all hosts read/none, or exactly one writer host
  and all others none. Therefore FIFO/control remains NC in v3.
- ARM64 is the production ISA. Cross-host FIFO ordering uses bus-domain fences,
  not CPU-domain shared-cache fences.

## Configuration

All knobs use the `UCX_OBMM_` prefix.

| Knob | Default | Meaning |
| --- | --- | --- |
| `MEM_MODE` | `nc` | `nc` or `hybrid`; pure `cc` is rejected |
| `NC_MEMIDS` | empty | Mandatory CSV of NC memids |
| `CC_MEMIDS` | empty | Mandatory in `hybrid`; warned/ignored in `nc` |
| `FIFO_SIZE` | `64` | FIFO ring depth, power of 2 |
| `FIFO_ELEM_SIZE` | `2048` | bytes per FIFO element, including 16B element header |
| `BCOPY_SEG_SIZE` | `4096` | v2 NC paired-desc size; max NC bcopy |
| `CC_CHUNK_SIZE` | `16k` | v3 hybrid CC chunk size; max hybrid bcopy |
| `FIFO_MAX_POLL` | `16` | RX completions per progress call |

Validation:

- `NC_MEMIDS` must be non-empty and contain exactly one local export.
- In `hybrid`, `CC_MEMIDS` must be non-empty and contain exactly one local CC
  export.
- A memid may not appear in both NC and CC lists.
- `CC_CHUNK_SIZE` must be non-zero, <= `UINT32_MAX`, page aligned, and fit in
  the per-slot CC slice.
- NC pool geometry must fit in the NC export region.

## Modes

### `MEM_MODE=nc`

This is the v2-compatible path:

- `am_short`: inline in the NC FIFO element.
- `am_bcopy`: payload in the paired NC `desc[N]` for FIFO element `N`.
- Pool version: `UCT_OBMM_POOL_VERSION_NC` (2).
- No CC mapping is used and `obmm_set_ownership()` is never called.

### `MEM_MODE=hybrid`

Hybrid keeps NC for all control and uses CC chunks only for bcopy payload:

- `am_short`: unchanged NC inline format.
- `am_bcopy`: sender packs into a local CC chunk, releases writer ownership,
  and publishes a small NC FIFO descriptor.
- Receiver takes read ownership of the matching CC import, synchronously
  invokes the AM callback, releases read ownership to `PROT_NONE`, then
  advances the NC FIFO tail.
- Pool version: `UCT_OBMM_POOL_VERSION_HYBRID` (3).

## NC pool and FIFO layout

The NC export starts with a pool header and per-slot metadata:

```text
offset 0
+-------------------------------------------------------+
| uct_obmm_pool_hdr_t                                   |
+-------------------------------------------------------+
| alloc_bitmap[bitmap_words]                            |
+-------------------------------------------------------+
| slot_meta[slot_count]                                 |
+-------------------------------------------------------+ hdr->slot_array_offset
| slot[0]:                                              |
|   uct_obmm_fifo_ctl_t                                 |
|   fifo_elem[fifo_size]                                |
|   bcopy_desc[fifo_size]  (NC mode data path)          |
+-------------------------------------------------------+
| slot[1] ...                                           |
```

`slot_stride = align_up(sizeof(ctl) + fifo_size * elem_size +
fifo_size * bcopy_seg_size, cacheline)`.

The pool header records version, mode, and hybrid chunk geometry. A process
must reject an existing pool whose version/mode/geometry does not match its
iface configuration.

## FIFO element format

`uct_obmm_fifo_element_t` stays 16 bytes:

| Field | Bytes | Meaning |
| --- | ---: | --- |
| `flags` | 1 | OWNER, BCOPY, and optional CC_CHUNK |
| `am_id` | 1 | AM id |
| `length` | 2 | short or NC-bcopy payload length |
| `generation` | 4 | receiver slot generation |
| `header` | 8 | short header, or hybrid CC descriptor metadata |

Formats:

- `am_short`: `BCOPY` clear, `CC_CHUNK` clear. `header` is the 8B AM header;
  inline payload begins at `elem + 1`; `length = sizeof(header) + payload`.
- NC `am_bcopy`: `BCOPY` set, `CC_CHUNK` clear. Payload lives in paired
  `desc[N]`; `length` is pack callback length; `header = 0`.
- Hybrid CC `am_bcopy`: `BCOPY | CC_CHUNK` set. `length` field is unused and
  `header` packs:

```text
bits  0..31  payload length
bits 32..47  absolute CC chunk index within sender CC export
bits 48..63  sender CC exporter table index
```

The CC exporter table is built by sorting configured CC exporter tuples. The
device/iface address carries a 64-bit table hash so peers reject mismatched
tables before decoding exporter indexes.

## Hybrid CC chunk pool

Each iface owns a deterministic slice of its local CC export:

```text
slot_slice_size      = floor(cc_export_size / UCT_OBMM_POOL_SLOT_COUNT)
slot_slice_base      = cc_export_base + slot_index * slot_slice_size
chunks_per_slot      = floor(slot_slice_size / CC_CHUNK_SIZE)
absolute_chunk_index = slot_slice_base / CC_CHUNK_SIZE + local_chunk_index
```

At hybrid iface init:

1. The local CC export slice is moved to `PROT_WRITE`.
2. A sender-private free stack is initialized with all absolute chunk indexes
   in the slice.
3. CC imports stay `PROT_NONE` until RX needs to read.

At cleanup:

1. Reclaim completed chunks where possible.
2. Move the local CC slice back to `PROT_NONE`.

There is no shared CC allocator in v3 and no mid-run dead-peer recovery; MPI
job restart is the recovery model.

## Sender algorithms

### FIFO reservation

Both short and bcopy reserve a peer NC FIFO slot with load + CAS, not FAA:

1. Read `peer_ctl->head`.
2. If `head - cached_tail >= fifo_size`, refresh `cached_tail` after a
   bus-load fence and retry the capacity check.
3. CAS `head` to `head + 1`.
4. If CAS loses, retry.

FAA cannot be used because a failed "full" check after FAA would leave a
non-rollbackable head gap, stalling the in-order receiver.

### NC `am_bcopy`

1. Reserve FIFO slot.
2. Pack directly into paired NC `desc[N]`.
3. Fill FIFO element metadata.
4. Bus-store fence.
5. Publish `flags = owner_bit | BCOPY`.

### Hybrid CC `am_bcopy`

1. Reclaim chunks whose FIFO descriptors were acknowledged by peer tail.
2. Check FIFO capacity and CC free chunk availability before calling
   `pack_cb`.
3. Pop one local CC chunk.
4. Pack directly into the CC chunk.
5. Release writer ownership with `PROT_NONE`.
6. Reserve the peer NC FIFO slot. If FIFO races full, restore the chunk to
   `PROT_WRITE`, push it back, and return `UCS_ERR_NO_RESOURCE`.
7. Fill FIFO descriptor with packed CC metadata.
8. Bus-store fence.
9. Publish `flags = owner_bit | BCOPY | CC_CHUNK`.
10. Record `(fifo_head, chunk_index)` in the ep in-flight queue.

The CC ownership release must complete before the NC FIFO element is
published.

## Receiver algorithm

`uct_obmm_iface_progress()` is the only RX progress engine:

1. Load the FIFO element at `read_index`.
2. Check OWNER bit for the expected ring lap; stop if not ready.
3. Bus-load fence.
4. Drop stale elements with mismatched generation.
5. Dispatch:
   - short: invoke AM with `&elem->header`, `length`, flags `0`
   - NC bcopy: invoke AM with paired `desc[N]`, `length`, flags `0`
   - hybrid CC bcopy:
     1. Decode length, chunk index, and exporter index.
     2. Validate descriptor bounds.
     3. Find the CC region by exporter index.
     4. Take `PROT_READ` ownership of the chunk.
     5. Invoke AM synchronously with flags `0`.
     6. Release read ownership to `PROT_NONE`.
6. Advance `read_index`.
7. After polling, issue a full bus fence before storing `recv_ctl->tail`.

The full bus fence before tail publication orders payload loads before the
tail store; a store-only fence is insufficient on aarch64.

## Reclaim and pending

Each hybrid ep keeps an ordered in-flight ring of `(fifo_head, chunk_index)`.
Reclaim reads peer tail and, for every entry with `fifo_head < peer_tail`,
restores the local chunk to `PROT_WRITE` and pushes it back to the iface free
stack.

Pending integrates both resources:

- NC FIFO slot availability.
- CC chunk availability in hybrid mode.

The existing arbiter is kept. Worker progress drains RX, publishes tails,
reclaims completed chunks, and dispatches pending sends.

## Addressing and reachability

Device address contains:

- NC exporter tuple.
- CC exporter tuple in hybrid mode.
- CC exporter table hash.

Iface address contains:

- slot index, generation, pid.
- FIFO/NC-bcopy geometry.
- mode and pool version.
- hybrid chunk geometry and local CC exporter index.

Reachability:

- NC mode: peer NC exporter tuple must map to a local export or import, and
  FIFO geometry/mode/version must match.
- Hybrid mode: NC requirements plus peer CC exporter tuple must map to a CC
  region; CC geometry and exporter table hash must match.

## Capabilities

| Capability | NC mode | Hybrid mode |
| --- | --- | --- |
| `AM_SHORT` | yes, `FIFO_ELEM_SIZE - 16` | same |
| `AM_BCOPY` | yes, `BCOPY_SEG_SIZE` | yes, `CC_CHUNK_SIZE` |
| `PENDING` | yes | yes |
| `CONNECT_TO_IFACE` | yes | yes |
| `CB_SYNC` | yes | yes |
| `INTER_NODE` | yes | yes |

PUT/GET, atomics, AM zcopy, AM dup, and EP check are not advertised.

## Verification without hardware

No real OBMM hardware is available in this workspace. Valid local checks are:

1. Build UCX with obmm.
2. Use `ucx_info -c` to confirm the OBMM config keys.
3. Use `ucx_info -d -t obmm` with valid OBMM env vars to inspect advertised
   caps.
4. Use `nm -D libuct.so | grep uct_obmm` for symbol sanity.

Cross-node MPI/OSU and ownership ordering validation require the real hardware
environment.
