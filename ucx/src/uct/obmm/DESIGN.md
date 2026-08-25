# OBMM UCT Transport Design

Status: the accepted transport is AM-only and NC-only. It exposes one logical
UCT TLS:

```text
obmm: NC AM over pre-exported OBMM shmdev blocks
```

UCP sees one capability and performance model. Each iface owns one NC receive
FIFO by claiming one local export block. Endpoints match peers by exporter
identity plus `region_id`; EP setup locates and maps the peer block it will
actually use. There is no secondary local-memory path.

Cross-node cacheable CC as a UCT data path was explored and rejected on
2026-06-05. Do not implement or tune staged CC zcopy, sender-owned CC,
receiver-owned CC, or CC batch/epoch cross-node paths unless a new design is
explicitly approved. Same-node cacheable direct AM was also removed from the
active transport on 2026-07-06; the supported runtime path is NC only.

---

## Design Intent And Reading Guide

This file is the normative description of the active transport. Code comments
explain local mechanics; `plan.md` records experiments and superseded designs;
the stable hardware and libobmm facts live in
`.github/skills/obmm-api-and-env/SKILL.md`. When these sources disagree about
the active transport, update the code and this file together rather than
copying historical behavior back into the implementation.

The design is deliberately narrow:

1. Provide an inter-node UCT Active Message transport over non-cacheable OBMM
   mappings.
2. Give every iface one independently owned receive FIFO in one export block.
3. Publish only the exporter tuple needed for peers to locate that FIFO.
4. Map peer blocks at EP creation, at the ownership boundary where future
   UCT-managed import will occur.
5. Expose only capabilities that have a complete implementation and a measured
   reason to exist.

The implementation is shaped toward a future deployment in which the iface
exports its own block and each EP imports the peer tuple when necessary. The
current deployment still prepares export/import shmdevs externally, so UCT
discovers and maps them but never calls libobmm export/import APIs. Keeping the
same md/iface/ep ownership boundaries now is more important than introducing a
process-wide mapping cache that would later have to be removed.

The following are explicit non-goals:

- supporting mixed obmm binaries or mixed FIFO geometry in one MPI job;
- discovering that ranks launched different UCX programs;
- exposing user buffers through OBMM, rkeys, RMA, atomics, or zcopy;
- cacheable ownership transitions or a same-node special path;
- recovering an MPI job after a rank dies in the middle of a FIFO operation;
- treating local memid values as cluster-visible identities.

### Mental Model

```text
one UCT md
  owns: local controller CNA/EID only
  does: stateless sysfs discovery on behalf of iface/EP operations

one UCT iface
  owns: one mapped + claimed local export block
        one receive FIFO in that block
        one fixed receive-descriptor pool in that block
        one local free list plus one replacement descriptor
        one pending-send arbiter
  publishes: (exporter_dcna, exporter_deid, region_id)

one UCT EP
  targets: the peer tuple published by the remote iface
  owns: one peer mapping when it opened that mapping itself
  borrows: the iface RX mapping only for self-loopback
  caches: peer block/FIFO pointers and peer tail
```

There is no MD region table, no memid allow-list, no import mmap cache, and no
shared allocator inside an export block.

---

## Environment Facts

- Export/import is done outside UCX. UCT must not call `obmm_export`,
  `obmm_unexport`, `obmm_import`, `obmm_unimport`, `obmm_preimport`, or
  `obmm_unpreimport`.
- A single MPI job is assumed to start the same UCX/obmm transport program
  and the same `UCX_OBMM_*` geometry configuration on every participating
  rank. Mixed UCX binaries, mixed obmm transport layouts, or mixed FIFO
  geometry inside one job are outside this UCT transport's responsibility; if
  such a launch is attempted, detection and reporting belong above UCT.
- The transport discovers OBMM identity and shmdev metadata through sysfs and
  maps `/dev/obmm_shmdev*` directly only at the component that needs the block.
- Local UB controller identity is discovered under
  `/sys/devices/ub_bus_controller*`. Controller attribute directories are
  children such as `00001` and are identified by a `ubc` marker file, matching
  libobmm's `*/ubc` anchored layout. Their `eid` and `primary_cna` files are
  libobmm-style non-negative int-sized scalar values that may be decimal or
  `0x`-prefixed hexadecimal. The ubmem EID protocol width is at most 20 bits,
  so UCT stores controller EID as a scalar value. OBMM shmdev
  `export_info/deid` and
  `import_info/deid` print EID as `u64 : u64`; in this environment the high
  half is invalid/unused and must be zero. UCT validates that form and stores
  the scalar value as DEID.
- The current discovery and mapping lifecycle is defined in
  "Discovery And Mapping Lifecycle" below.
- NC mappings are opened as `open(..., O_RDWR | O_SYNC)` and mapped with
  `MAP_SHARED | PROT_READ | PROT_WRITE`.
- Do not call `obmm_set_ownership()` from the UCT transport. It is irrelevant
  for NC.
- Peer matching is by exporter identity, not memid. Use exporter DCNA/DEID
  from sysfs `export_info`/`import_info` plus the transport `region_id`
  parsed from shmdev `priv=ucx-obmm:NN` metadata when multiple blocks share an
  exporter.
- On arm64 NC mappings, shared control-word atomic RMW must use explicit LSE
  instructions. Do not rely on compiler-lowered LL/SC atomics or generic
  `ucs_atomic_*` for shared NC control words.

---

## Code Map And Ownership

| File | Responsibility | Must not own/do |
| --- | --- | --- |
| `obmm_md.c/.h` | Register the component and MD, read local CNA/EID, perform fresh sysfs discovery and exact tuple lookup. | No shmdev mmap table, no export/import API calls, no rkey/user-memory support. |
| `obmm_sysfs.c/.h` | Parse controller identity and transport-labelled shmdev metadata into `uct_obmm_dev_info_t`. | No mappings and no transport policy beyond strict candidate filtering. |
| `obmm_region.c/.h` | Own one `open` + `mmap` lifetime for one shmdev. | No peer matching, FIFO initialization, or claim policy. |
| `obmm_block.c/.h` | Validate one-block layout, claim/reclaim an export block, publish/read `fifo_offset`, keep the block non-READY during owner initialization, and release an owned block. | No UCT object lifecycle or AM dispatch. |
| `obmm_fifo.h` | Define shared FIFO control/element layout, pointer math, short capacity, descriptor offsets, and bus-fence helpers. | No allocation or ownership policy. |
| `obmm_iface.c/.h` | Validate runtime geometry, claim one RX export, initialize/manage its fixed bcopy receive-buffer pool, advertise UCT capabilities/address, progress RX, and dispatch pending sends. | No process-wide peer mapping cache. |
| `obmm_ep.c/.h` | Resolve/map one peer tuple, reserve peer FIFO elements, publish inline AM short or descriptor-backed AM bcopy, and own the per-EP pending group. | No local RX claim and no libobmm import/export call. |
| `obmm_atomic.h` | Provide the explicit shared-control CAS implementation required by NC mappings on arm64. | No generic data-path ordering policy. |

### Resource Ownership Table

| Resource | Owner | Borrowers | Release point |
| --- | --- | --- | --- |
| local CNA/EID | MD | iface discovery and peer lookup | `md_close` |
| temporary discovered-device array | current discovery call | none | before that call returns |
| local RX export fd/mmap | iface | self-loopback EP may borrow it | iface cleanup, after EPs are destroyed |
| local export block claim | iface | peers only observe READY FIFO/descriptor data | iface cleanup via `uct_obmm_block_release()` |
| fixed bcopy receive buffers in local export | iface | remote senders write only the buffer offset currently published by a FIFO slot | block release with the RX export |
| bcopy descriptor returned as `UCT_CB_PARAM_FLAG_DESC` | UCP after its callback returns `UCS_INPROGRESS` | none | `uct_iface_release_desc()` returns it to the iface free list |
| remote import/export fd/mmap | EP that opened it | that EP only | EP cleanup |
| self-loopback peer mapping | iface | self-loopback EP | EP does not unmap; iface cleanup owns it |
| pending request | UCP while queued in EP group | iface arbiter dispatch | callback success or EP purge |

Fresh discovery is intentional. `uct_obmm_md_find_device()` scans current
sysfs state and requires exactly one match for the peer tuple. A duplicate
tuple is an error, not a reason to choose the lowest memid. Discovery results
are sorted by memid only to make local export claim order deterministic; memid
never participates in peer identity.

---

## Discovery And Mapping Lifecycle

The current stage target is to move the externally prepared deployment toward
the future UCT-managed export/import shape: one process exports/maps its own
RX block, publishes the exporter tuple, and peers resolve/import/map only the
tuple needed for a connection. While export/import are still prepared outside
UCX, the active transport uses private metadata discovery only; there is no
user-supplied memid allow-list.

### Candidate Discovery

`uct_obmm_sysfs_discover()` scans `/sys/devices/obmm/obmm_shmdev*`. A directory
is a candidate only when all of the following hold:

- its suffix is a nonzero decimal memid;
- `priv_len` and `priv` describe exactly `ucx-obmm:NN` with two decimal digits;
- `type` is `export` or `import`;
- `size` is readable and nonzero when mapped;
- `allow_mmap` is nonzero;
- the corresponding export/import identity fields are present and valid.

`NN` becomes a 32-bit `region_id` in memory, but the current private format
admits only `00` through `99`. For exports, discovery reads DEID from
`export_info/deid` and the MD supplies the local controller CNA. For imports,
both exporter CNA and DEID come from `import_info`. A local memid is retained
only to form `/dev/obmm_shmdev<memid>` and to provide diagnostics.

### End-To-End Sequence

```text
md_open
  read one local controller child marked by `ubc`
  store local_cna + local_eid
  do not enumerate or mmap shmdevs

iface_init
  validate FIFO/performance configuration
  discover current transport-labelled shmdevs
  iterate exports in memid order
    open + mmap candidate
    compute region_id-based fifo_offset for FIFO plus descriptor-pool span
    try to claim block
    occupied live claim -> close and try next export
    while claim is non-READY, initialize FIFO descriptor offsets and free list
    publish READY only after all shared receive state is usable
  retain exactly one successful export mapping and claim
  advertise that block's exporter tuple as the device address

iface_is_reachable_v2(peer device address)
  if tuple equals iface-owned RX export -> reachable self-loop candidate
  else perform fresh exact tuple lookup in sysfs
  reject missing or ambiguous tuples
  do not open or mmap the peer block

ep_create(peer device address)
  resolve the same tuple again
  self-loop -> borrow iface RX mapping
  otherwise -> open + mmap the resolved local shmdev into EP storage
  open the READY block header using local job geometry
  cache peer ctl/element pointers, tail, and tuple

progress
  drain the iface-owned RX FIFO
  publish receive tail
  dispatch pending send retries

ep_cleanup
  purge its pending group
  close only an EP-owned peer mapping

iface_cleanup
  runs after EP destruction
  release claim, clear block, unmap local RX export
  clean up iface arbiter
```

UCT class construction has a separate failure path from `iface_cleanup`.
If `iface_init` returns an error after it has acquired an obmm-owned resource,
it releases that resource before returning. UCX then cleans up only the parent
classes whose constructors completed; it does not invoke the failed
`uct_obmm_iface_t` constructor's cleanup function. Conversely,
`iface_cleanup` runs only for a successfully constructed iface, so it may
unconditionally disable base progress, release the RX block, and clean up the
arbiter. The iface therefore carries no per-subresource `*_initialized`
flags solely for constructor rollback.

The ownership boundary is:

1. `md_open` discovers only local controller identity and lightweight sysfs
   access helpers. It does not mmap shmdevs, does not enumerate a process-wide
   region table for later endpoint lookup, and does not own import mappings.
2. `iface_init` finds candidate local export shmdevs by transport private
   metadata, maps exactly one usable export block, initializes/claims that
   block's FIFO header, and owns this RX export mapping until iface cleanup.
3. `iface_is_reachable_v2` checks that the peer exporter tuple can be resolved
   through sysfs. It must not mmap the peer block and must not depend on an MD
   pre-mapped region list.
4. `ep_create` resolves the peer-published `(dcna, eid, region_id)` tuple to
   the local shmdev that represents the peer FIFO, then maps that one block and
   opens the peer FIFO header. For remote peers this is an import shmdev; for
   same-node peers it may be another local export shmdev; for self-loopback it
   may reuse the iface-owned export mapping.
5. EP cleanup releases only mappings that the EP created. It must not unmap
   the iface-owned RX export mapping.

There is intentionally no MD-owned `opened_imports` table, import mmap cache,
or process-local mmap refcount in this design. A UCT process is allowed to map
the peer block it needs at EP creation time and release that EP-owned mapping
when the EP is destroyed. Cross-process mmap state is outside UCT's address
space and cannot be represented by a process-local refcount.

The current implementation also does not deduplicate mappings across EPs.
That choice follows the accepted workload assumption that a process does not
create duplicate EPs to the same peer tuple. If that assumption changes, any
mapping cache must be designed at the EP/import ownership boundary and must
not move all peer discovery or mapping back into `md_open`.

This lifecycle intentionally keeps discovery and mapping at the same component
boundaries that future UCT-managed export/import will use.

---

## Capabilities

`obmm` advertises:

| Capability | Status | Notes |
| --- | --- | --- |
| `AM_SHORT` | yes | payload remains inline in the small FIFO element |
| `AM_BCOPY` | yes | FIFO carries a block-relative offset to a separate receive buffer |
| `PENDING` | yes | arbiter-backed retry on FIFO backpressure |
| `CONNECT_TO_IFACE` | yes | endpoint uses peer device address; iface address is empty |
| `CB_SYNC` | yes | AM callbacks are invoked only from `uct_worker_progress()` context |
| `INTER_NODE` | yes | Advertised when the iface owns its NC RX FIFO |

The ops table also supports `AM_SHORT_IOV` through the UCX base helper, which
packs the iov into the existing FIFO-backed `AM_SHORT` operation. This does not
add a separate capability flag or zero-copy data path.

Valid bcopy receives invoke the AM callback with
`UCT_CB_PARAM_FLAG_DESC`. If the callback returns `UCS_OK`, the FIFO slot keeps
the same receive buffer. If it returns `UCS_INPROGRESS`, UCP owns that buffer
until `uct_iface_release_desc()` and the receiver replaces the FIFO slot's
buffer offset before releasing the slot to producers. Inline short receives do
not carry `UCT_CB_PARAM_FLAG_DESC` and remain callback-lifetime data.

The internal ops provide diagnostics only: VFS refresh exposes local obmm FIFO
block state, while endpoint query succeeds only for an empty field mask and
returns unsupported for sockaddr fields because obmm endpoints do not have
socket addresses.

Not advertised: `AM_ZCOPY`, PUT/GET/RMA, atomics, `EP_CHECK`,
AM_DUP, and ERRHANDLE_PEER. The ops table must keep unsupported stubs for
unsupported entries.

The MD also does not support UCX user-buffer memory management or remote-key
semantics: `mem_alloc`, `mem_free`, `mem_advise`, `mem_reg`, `mem_dereg`,
`mem_query`, `mem_attach`, `detect_memory_type`, `mkey_pack`, component
`rkey_unpack`, `rkey_ptr`, and `rkey_release` all return unsupported. OBMM only
maps the externally prepared transport FIFO regions; it never registers or
packs user send/receive buffers.

### Why The Capability Surface Is Narrow

The transport mmap is an implementation detail for its FIFO, not a promise
that arbitrary peer virtual addresses can be dereferenced. In particular,
`UCT_MD_FLAG_NEED_RKEY`, `rkey_ptr`, or user-memory registration would let UCP
select rendezvous/RMA paths whose pointer semantics OBMM does not implement.
Keeping MD flags and rkey operations unsupported prevents UCP from treating a
transport FIFO mapping as access to a peer process's heap.

`CONNECT_TO_IFACE` means UCP creates an EP from the peer device address. The
iface address is intentionally zero bytes and the EP address is also zero
bytes. `CB_SYNC` describes callback invocation context, not descriptor
lifetime: callbacks run from `uct_worker_progress()`, while a descriptor-backed
bcopy buffer may outlive the callback after `UCS_INPROGRESS`. `INTER_NODE` is
advertised only after the iface successfully claims and fully initializes its
RX export, because without that block the iface cannot receive cross-node AM
traffic.

The base helper implements `AM_SHORT_IOV` by packing into the existing
`AM_SHORT` operation. It does not create another wire format or capability
bit. Conversely, an operation must not be added to the ops table without its
matching `iface_query()` flag and numeric limits, and a flag must not be
advertised without a complete operation implementation.

OBMM does not advertise `UCT_IFACE_FLAG_ERRHANDLE_BCOPY_LEN`. The UCP path is
responsible for honoring the advertised bcopy bound; the optional
`UCT_CHECK_LENGTH()` in parameter-check builds is diagnostic, not a supported
error-recovery capability.

---

## FIFO Block Layout

The current hardware environment pre-provisions one shmdev export block per
process, so process fanout comes from multiple local exports rather than from
sub-allocating one large export. Each block contains one FIFO. There is no
shared allocator, bitmap, per-FIFO allocation metadata, or FIFO index in the
active layout.

```text
fifo_stride       = align(fifo_control + FIFO_SIZE * FIFO_ELEM_SIZE, 64)
desc_payload_off  = release-word + UCT rx_headroom
desc_stride       = align(desc_payload_off + BCOPY_SEG_SIZE,
                          requested AM alignment)
descriptor_bytes  = RX_DESC_COUNT * desc_stride
required          = colored_fifo_offset + fifo_stride
                    + descriptor-alignment slack + descriptor_bytes
```

The concrete block layout is:

```text
region base
  +0   u64 claim
  +8   u64 fifo_offset
  ...  header-to-fifo alignment/slack

fifo_offset (64-byte aligned and stored in the shared block header)
  cacheline 0: producer-owned head
  cacheline 1: consumer-owned tail
  elements[FIFO_SIZE], each FIFO_ELEM_SIZE bytes

one element
  +0   u8  flags       (OWNER and BCOPY)
  +1   u8  am_id
  +4   u32 length
  +8   u64 bcopy payload offset from region base (persistent across short)
  +16  u64 short header; short payload follows at +24
  ...  remaining inline short bytes through FIFO_ELEM_SIZE

descriptor pool (aligned after the FIFO)
  RX_DESC_COUNT fixed-stride receive buffers

one receive buffer
  UCT release-word storage
  UCT-requested rx_headroom
  BCOPY_SEG_SIZE payload bytes at the requested AM alignment
```

`uct_obmm_block_t` and the local descriptor free-list metadata are not shared
ABI. The shared ABI is the block header, FIFO control words, FIFO element
bytes, and the meaning of each block-relative bcopy payload offset. A peer does
not consume the receiver's local release word, free-list indices, or virtual
pointers.

`FIFO_ELEM_SIZE` now contains only FIFO metadata plus inline short bytes.
`am_short` stores `[header | payload]` starting at byte 16 and does not modify
the descriptor field. `am_bcopy` packs into a separate receive buffer whose
block-relative payload offset is stored persistently at byte 8. Keeping the two
fields separate is required: a short publication must not destroy the buffer
assignment needed when the same FIFO slot later carries bcopy. This keeps
mapping-local virtual addresses off the wire and lets the receiver replace one
buffer without moving or retaining the FIFO slot itself.

This follows MM's descriptor handoff but not its dynamic allocation backend.
MM can grow receive chunks through its MD-backed mpool; obmm maps one externally
provisioned fixed-size block and its MD intentionally does not implement memory
allocation. OBMM therefore places a fixed descriptor array in that block and
keeps only its free-list indices in process-local cacheable memory. Runtime
pool exhaustion becomes FIFO backpressure rather than a new OBMM allocation or
libobmm API call.

Current defaults:

```text
FIFO_SIZE       = 256
FIFO_ELEM_SIZE  = 128
BCOPY_SEG_SIZE  = 65536
RX_DESC_COUNT   = 512
FIFO_MIN_POLL   = 64
FIFO_MAX_POLL   = 128
PENDING_QUOTA   = 1
short_capacity  = 112 total AM bytes (8-byte header + 104-byte payload)
max_short       = 112 total AM bytes
max_bcopy       = 65536 bytes
```

### Geometry Invariants

- `FIFO_SIZE` is nonzero and a power of two; `fifo_mask` is
  `FIFO_SIZE - 1`.
- `FIFO_ELEM_SIZE` is at least the 24-byte FIFO element header and is large
  enough for the 8-byte UCT short header at byte 16.
- `BCOPY_SEG_SIZE` is at least 64 bytes for UCP; it is independent of
  `FIFO_ELEM_SIZE` and must fit in every descriptor payload.
- `RX_DESC_COUNT` is greater than `FIFO_SIZE`. Initialization assigns one
  descriptor to every FIFO slot and retains the remainder as replacement
  capacity; the default has 256 attached and 256 replacement descriptors.
- `max_short` is `min(FIFO_ELEM_SIZE - 16, 131184)` and includes the 8-byte
  UCT short header.
- Every descriptor payload honors `UCT_IFACE_PARAM_FIELD_AM_ALIGNMENT` and
  `UCT_IFACE_PARAM_FIELD_AM_ALIGN_OFFSET`. The release word immediately before
  rx headroom remains mapping-local state; only the aligned payload offset is
  published in a FIFO element.
- FIFO/layout strides use `size_t`, while the shared descriptor payload offset
  is a 64-bit block-relative value. Layout helpers reject arithmetic overflow,
  invalid offsets, and regions smaller than the complete FIFO-plus-pool span.
- All ranks in one job use the same geometry. Peer blocks are opened with
  local iface geometry; geometry is not carried or negotiated on the wire.

With defaults, the 128-byte FIFO elements produce a FIFO stride of 32,896
bytes. On a 64-bit build with zero UCT rx headroom and default 64-byte AM
alignment, each 65,536-byte payload occupies a 65,600-byte descriptor stride;
512 descriptors occupy 33,587,200 bytes. Including the minimum FIFO offset and
descriptor alignment padding, the minimum required region is approximately
33,620,216 bytes. Coloring can select at most eight 64-byte positions inside
the low-9-bit window; the largest default offset still fits in a 34 MiB block.
The constructor computes the exact requirement from the actual UCT rx
headroom/alignment and rejects a block that is too small.

The default layout therefore remains above 32 MiB and, with the current 2 MiB
OBMM allocation granularity, each export block remains provisioned as 34 MiB.
A 96-process node still uses 96 local export blocks, for 3264 MiB of local NC
export capacity. One `UCX_OBMM_*` geometry configuration applies to the
receive FIFO and descriptor pool.

As of the 2026-07-07 small-message regression fix, the FIFO is no longer
placed at the same block-relative offset in every export/import block. The
FIFO block header remains fixed at the region base, but `fifo_offset` is
chosen from the 34 MiB block's spare space after reserving both the FIFO and
descriptor-pool span. The current hardware decoder
is sensitive to identical PA low 9 bits. Because the FIFO base stays 64-byte
aligned, the color selection rotates FIFO bases through the usable low-9-bit
values by `region_id`. The block's spare space only has to be large enough
for this low-bit offset adjustment. Target OSU measurements showed this
restored the old single-region small-message performance by avoiding identical
FIFO-control offsets across many independent shmdev blocks.

Coloring is intentionally selected from the FIFO base. The descriptor pool is
then alignment-placed after that FIFO within the already reserved full-layout
span; coloring does not alter element/descriptor strides, device addresses, or
`region_id`. The selection is `region_id modulo usable_color_positions`,
capped by the eight cacheline positions in the low-9-bit window. If there is no
spare space, the code falls back to the minimum header-aligned FIFO offset.

### Export Block Claim Protocol

The shared 64-bit `claim` word combines a pid/starttime owner token with a
READY bit. Start time prevents PID reuse from making a dead owner appear live.
The valid transitions are:

```text
0 (free)
  -- CAS --> self token (claimed, initialization in progress)
  -- clear region except claim; write fifo_offset; initialize FIFO and
     descriptor offsets while claim remains non-READY
  -- store fence --> self token | READY

self token | READY
  -- owner CAS --> self token
  -- clear region except claim; store fence --> 0

self token (constructor rollback before READY)
  -- clear region except claim; store fence --> 0
```

An invalid token is reset and retried. A valid token whose pid/starttime is
live is occupied, regardless of whether READY is set; iface attach does not
wait for another live initializer. A dead READY or non-READY owner may be
taken over with CAS, after which the new owner clears and reinitializes the
whole block. Claim publication happens before clearing so a second process
cannot initialize the same export concurrently.

`uct_obmm_block_attach()` is the owner path used by iface initialization. It
may claim, recover, and clear a block, but deliberately leaves the claim
non-READY. The iface initializes all FIFO-to-buffer offsets and then calls
`uct_obmm_block_publish_ready()`. Constructor rollback releases either a
non-READY or READY self claim. `uct_obmm_block_open()` is the peer path used by
EP creation. It only accepts a valid READY header and derives pointers from the
published `fifo_offset`; it never changes ownership.

Receive polling starts at 64 completions and adaptively grows to 128 when
successive progress calls consume the complete poll window. A low-traffic call
still stops at the first unpublished FIFO element. The 128 upper bound drains
at most half of the 256-entry ring before publishing `tail` and dispatching
pending sends, avoiding a full-ring callback batch that would delay both.

The adaptation is additive-increase/multiplicative-decrease: two consecutive
full windows increase the poll count by one up to `FIFO_MAX_POLL`; a partial
window halves it, bounded by `FIFO_MIN_POLL`. This changes progress batching,
not FIFO capacity or wire format.

Prefer 64-byte-aligned `FIFO_ELEM_SIZE`, `BCOPY_SEG_SIZE`, and resulting
descriptor strides unless new target measurements prove otherwise.
Non-64B-aligned strides have regressed latency on the current platform.

---

## Address And FIFO Format

The transport no longer carries a peer ABI or FIFO-geometry guard in the UCT
iface address. The job-level same-UCX assumption above is the compatibility
contract. FIFO elements retain `length@4`, add the persistent block-relative
bcopy payload offset at byte 8, and retain the inline short header at byte 16.

`uct_obmm_device_addr_t` carries:

```text
offset  size  field
0       8     exporter_dcna
8       4     exporter_deid
12      4     region_id
```

`uct_obmm_iface_addr_t` carries:

```text
no bytes; iface_addr_len is 0
```

Region addresses include exporter DCNA, exporter DEID, plus a 32-bit
`region_id` parsed from shmdev `priv=ucx-obmm:NN` metadata. Regions without
transport private metadata are not transport candidates. The device address is
16 bytes and the iface address is 0 bytes, fitting worker-address v1's limits.
`ep_create` uses the local iface geometry for peer FIFO pointer math because the peer is
assumed to run the same transport code and configuration in the same MPI job.

The tuple means “the export block owned by this remote iface,” not “a local
shmdev.” On the exporting node it resolves to an export shmdev; on another node
it normally resolves to the corresponding import shmdev; on self-loopback it
resolves to the iface-owned export mapping. Those local objects may have
different memids while representing the same tuple.

The wire address deliberately omits pid, memid, FIFO index, geometry, claim
token, and local virtual address:

- pid/starttime is local block-claim state, not peer identity;
- memid is host-local and may be reordered between export and import;
- there is one FIFO per block, so no FIFO index exists;
- geometry compatibility is guaranteed by the same-program job assumption;
- claim and virtual pointers are mapping-local implementation details.

Changing `uct_obmm_device_addr_t`, the private metadata format, or any shared
FIFO field/offset interpretation is a protocol change even though there is no
explicit wire-format version field. Such a change requires one binary and
configuration across the whole job and synchronized updates to address
lengths, reachability, EP setup, layout validation, and this document.

---

## Reachability

MD provides local identity only. The local iface owns one claimed export block.

For each peer:

1. Resolve the peer primary exporter identity plus `region_id` through sysfs
   to the local shmdev representing the peer FIFO.
2. `iface_is_reachable_v2` performs only this resolution check; it
   does not mmap the peer block.
3. `ep_create` maps the resolved block as described in
   "Discovery And Mapping Lifecycle" and opens the peer FIFO header.
4. If no matching shmdev exists, the peer is unreachable.

Memid is never used as the peer key, so local memids may be in a different
order from the remote node's imports as long as the exported `priv` metadata
produces the same `region_id`.

---

## Send Path

The shared FIFO is multi-producer and single-consumer. Every EP that targets
an iface's published tuple may contend on that iface's `head`; only the owner
iface advances `tail`. Producers therefore reserve an absolute head index with
CAS:

1. Read shared `head`.
2. Compare `head - cached_tail` with `FIFO_SIZE`.
3. If apparently full, issue a bus load fence, refresh shared `tail`, and
   return `UCS_ERR_NO_RESOURCE` if the ring is still full.
4. CAS `head` to `head + 1`; retry from the new shared value after contention.
5. Use `head & (FIFO_SIZE - 1)` to select the physical element.

Fetch-and-add is not valid here. If it increments `head` and only afterwards
discovers a full ring, the producer cannot roll the reservation back and the
consumer will eventually stop forever at that unpublished index. The explicit
arm64 LSE CAS is therefore both a hardware requirement and a FIFO correctness
requirement.

`am_short` reserves one peer FIFO element with explicit LSE CAS on peer `head`.
It writes the FIFO element, copies payload inline after the AM header field,
issues a bus-domain release fence, and publishes the owner bit.

`am_bcopy` reserves one peer FIFO element, orders its descriptor-offset load
after the tail value that made the slot reusable, and resolves that
block-relative offset against the peer block mapping. It packs into the
separate receive buffer, writes AM id/length without changing the selected
descriptor offset, issues the same bus-domain release fence, and publishes a
FIFO element with the bcopy flag. The supported UCP path limits every pack
operation from the advertised `cap.am.max_bcopy`, which equals
`BCOPY_SEG_SIZE`. If the configured descriptor count/stride and payload size do
not fit in the export block, iface open fails during geometry validation.

After `pack_cb` returns, the send path uses the standard `UCT_CHECK_LENGTH()`
parameter check against `BCOPY_SEG_SIZE`. Parameter-check builds log and
return `UCS_ERR_INVALID_PARAM` for a violated UCP/UCT contract; release builds
configured with `--disable-params-check` omit the check. This diagnostic is not
a recoverable FIFO error path because the head was already reserved, and the
transport does not advertise `UCT_IFACE_FLAG_ERRHANDLE_BCOPY_LEN`. The
same-program UCP job assumption makes this branch unreachable in the supported
runtime path.

The element publication order is part of the protocol:

```text
reserve absolute head
for bcopy: acquire the slot's published descriptor offset after tail
write inline short data or the selected bcopy buffer
write per-message metadata (am_id and length; short also writes header)
bus store fence
write flags last (OWNER plus optional BCOPY)
```

The OWNER bit alternates on each traversal of the ring. For absolute index
`head`, indices with `head & FIFO_SIZE == 0` publish OWNER=1 and the next lap
publishes OWNER=0. This lets the receiver distinguish a newly published entry
from stale bytes without clearing every slot after consumption.

All fences are NC bus-domain fences because cross-host non-cacheable memory
visibility depends on bus ordering. Their roles are distinct:

- producer store fence: payload and metadata become visible before `flags`;
- consumer load fence: observing `flags` happens before reading payload;
- consumer full fence: callback payload loads and a replacement descriptor
  offset store complete before publishing a larger `tail`, so a producer
  cannot reuse the element or observe the old buffer too late;
- producer load fence: the `tail` load that authorizes reuse is ordered before
  reading the FIFO slot's replacement descriptor offset;
- `ep_fence` and `iface_fence`: apply the full bus-domain fence requested by
  the UCT API.

If the peer FIFO is full, send returns `UCS_ERR_NO_RESOURCE`. `pending_add`
returns `UCS_ERR_BUSY` only when TX space is already available and that EP has
no older queued request, telling UCP to retry the operation directly. Otherwise
it appends the request to the EP's arbiter group. Queueing behind an existing
group preserves per-EP send order even if the FIFO happens to have space at
the instant of `pending_add()`.

Each iface progress call drains RX first and then dispatches pending work. The
pending callback refreshes peer `tail` before invoking the UCP request. Success
removes the request, `UCS_INPROGRESS` advances to another group, and a
transient failure reschedules the group. `PENDING_QUOTA` limits successful or
in-progress callbacks counted in one dispatch; the current arbiter call also
dispatches one group unit per progress invocation, so changes above the
default value of one require code-path verification, not just configuration
tuning.

`ep_flush` and `iface_flush` complete immediately because the AM path has no
locally outstanding asynchronous operation once a FIFO element has been
published. This does not mean the remote AM callback has run; it means the
transport has completed local publication according to its UCT contract.

---

## Receive And Progress

The per-iface receive path consumes entries strictly in absolute
`read_index` order:

1. Derive the physical slot and expected OWNER bit from `read_index`.
2. Stop at the first slot whose OWNER bit is not the expected value.
3. After observing the expected OWNER, issue the bus load fence.
4. For short, validate and invoke directly on inline FIFO bytes without
   descriptor ownership.
5. For bcopy, validate the length and descriptor offset, ensure one replacement
   descriptor is available, and invoke on the separate buffer with
   `UCT_CB_PARAM_FLAG_DESC`.
6. If bcopy returns `UCS_OK`, keep the same buffer on the FIFO slot. If it
   returns `UCS_INPROGRESS`, mark that buffer UCP-owned and replace the slot's
   offset before advancing. If no replacement is available, stop before
   invoking/consuming that bcopy slot so FIFO backpressure protects ownership.
7. Increment `read_index`; the element itself is not cleared.
8. After the batch, issue a full bus fence and publish `tail=read_index`.
9. Adjust the next poll window and then let iface progress dispatch pending
   sends.

Strict ordering means later published elements cannot bypass an unpublished
earlier reservation. This is acceptable under the process-liveness model
below: a producer process that dies after reserving `head` terminates the MPI
job; the transport does not attempt to repair the resulting hole.

Inline short callback data is valid only for the callback lifetime because
publishing `tail` allows a producer to reuse the FIFO slot. Bcopy callback data
belongs to the separate descriptor pool. When UCP retains it, the old buffer
is no longer named by any reusable FIFO slot and is returned to the local free
list only through `uct_iface_release_desc()`. Thus advancing `tail` can recycle
the FIFO slot without recycling the UCP-owned buffer.

The pool is finite by design. The default 512 descriptors provide one buffer
for each of 256 FIFO slots plus 256 possible replacements. If UCP holds every
replacement, RX progress stops at the next valid bcopy publication until a
release callback returns a buffer. Short publications earlier in FIFO order
still complete, but strict FIFO ordering does not allow later short messages to
bypass the blocked bcopy. This converts descriptor exhaustion into bounded
transport backpressure rather than a copy fallback or use-after-reuse.

Malformed receive lengths are logged and the callback is skipped, but the
receiver still advances `read_index` and `tail`. This keeps a bad element from
permanently wedging progress; it is diagnostic containment, not peer error
recovery or a promise that the MPI job can continue correctly.

OBMM uses one UCX per-iface progress callback. It polls the single NC FIFO and
then dispatches pending sends. Poll-window adaptation is described under
"FIFO Block Layout". Export ownership and crash-time takeover are separate
from FIFO message progress and are described under "Export Block Claim
Protocol".

---

## Failure Model And Recovery Boundary

The transport distinguishes setup failures, ordinary backpressure, stale
block ownership, and failures that invalidate the whole MPI job:

| Condition | Behavior | Recovery boundary |
| --- | --- | --- |
| no usable local controller identity is found | MD open fails with a diagnostic | fix sysfs/environment, then restart |
| no eligible export or every export has a live claim | iface open fails | ensure enough pre-exported blocks or fewer local ranks |
| peer tuple missing or duplicated | reachability fails; EP cannot be created | fix control-plane import/export provisioning |
| shmdev open/mmap or FIFO/descriptor-pool geometry validation fails | owning iface/EP construction unwinds its resources | fix permissions, block size, headroom/alignment, or configuration |
| peer FIFO temporarily full | send returns `UCS_ERR_NO_RESOURCE`; UCP may queue via pending | normal runtime backpressure |
| all replacement receive descriptors are UCP-owned | RX stops before consuming the next bcopy slot; FIFO backpressure propagates until `uct_iface_release_desc()` | normal bounded descriptor backpressure |
| dead process left a block claim | next iface may CAS-take over and reinitialize the complete block | supported between runs/failed owners |
| producer dies after reserving FIFO head | receiver can stop at the unpublished index | no transport recovery; MPI job is expected to exit |
| FIFO carries an invalid bcopy descriptor offset | internal shared-protocol corruption; sender must not pack through it | no recovery; same-binary job invariant was violated |
| mixed binary or geometry in one job | behavior is undefined by this transport | rejected launch; detection belongs above UCT |

The claim protocol can recover ownership of an abandoned block because the
entire block is reinitialized before READY is republished. It cannot recover a
single abandoned FIFO reservation while keeping other producers alive: the
FIFO element has no producer identity or per-message lease, and adding those
would change the shared protocol and hot path. This is intentional under the
accepted MPI failure model.

A direct UCT caller can violate the advertised bcopy callback contract after
`head` has been reserved. Parameter-check builds diagnose that condition, but
there is no rollback path and OBMM does not advertise length-error recovery.
The supported UCP path bounds bcopy before invoking UCT. Receive-side length
checks prevent an invalid entry from blocking the FIFO, but cannot restore the
semantic correctness of a job that sent malformed data.

---

## Future UCT-Managed Export/Import Direction

The current code must remain easy to evolve from externally provisioned
shmdevs to UCT-managed libobmm calls without moving responsibilities between
layers. The target ownership shape is:

```text
md_open
  discover local controller CNA/EID only

iface_init
  export one block for this process
  mmap and claim/initialize its one FIFO
  publish (dcna, deid, region_id)

ep_create(peer tuple)
  look for an already available local shmdev with that exact tuple
  if absent, import exactly that tuple through libobmm
  mmap and open the peer FIFO

cleanup
  EP releases imports/mappings it created
  iface releases its own export/mapping
```

No current libobmm export/import call is implied by this section. Introducing
those calls requires a separate design update covering API inputs, failure
rollback, import identity, concurrent EP creation, and unimport/unexport
ordering. A cache or refcount should be added only if the future implementation
demonstrates duplicate tuple use inside one process; it belongs at the
EP/import boundary and must distinguish borrowed pre-existing shmdevs from
imports created by UCT.

The invariants that should survive that evolution are: one iface owns one RX
block, peers identify it by exporter tuple rather than memid, MD does not map
all transport memory, reachability does not mmap, and EP construction acquires
only the peer resource needed by that connection.

---

## UCP Tuning Hooks

`uct_obmm_iface_estimate_perf()` is retained for the single TLS.
It reports:

- `UCT_EP_OP_AM_SHORT`: configured bandwidth plus `SHORT_OVERHEAD`.
- `UCT_EP_OP_AM_BCOPY`: configured bandwidth plus `BCOPY_OVERHEAD`.

Only AM short and bcopy are advertised and expected to reach this callback.
The current callback initializes unknown operation values with the short
model and returns `UCS_OK`; this fallback is not an additional capability and
must not be used to justify advertising another operation. If a new operation
is added, give it an explicit model or explicitly reject it in the same code
change.

### Configuration Contract

All active transport knobs use the `UCX_OBMM_*` prefix:

| Knob | Default | Design effect |
| --- | --- | --- |
| `BW` | `3400MBs` | UCP bandwidth cost model only |
| `SHORT_OVERHEAD` | `1800ns` | UCP AM-short cost model only |
| `BCOPY_OVERHEAD` | `2us` | UCP AM-bcopy cost model only |
| `FIFO_SIZE` | `256` | shared ring depth, mask, stride, and owner-bit lap |
| `FIFO_ELEM_SIZE` | `128` | shared element stride and physical inline-short capacity |
| `BCOPY_SEG_SIZE` | `65536` | advertised bcopy limit and payload bytes in each receive descriptor |
| `RX_DESC_COUNT` | `512` | fixed receive-buffer count; must exceed `FIFO_SIZE` |
| `FIFO_MIN_POLL` | `64` | lower bound of adaptive receive batch |
| `FIFO_MAX_POLL` | `128` | upper bound of adaptive receive batch |
| `PENDING_QUOTA` | `1` | pending retry work attempted from progress |

`FIFO_SIZE`, `FIFO_ELEM_SIZE`, `BCOPY_SEG_SIZE`, and `RX_DESC_COUNT`
participate directly in shared-memory layout and must match across the job.
UCT rx headroom/alignment also affects local descriptor placement and must fit
the block, though peers consume the published payload offsets rather than
recomputing them. Polling and pending knobs do not change shared bytes, but the
accepted launch contract still assumes one common `UCX_OBMM_*` configuration.
Performance knobs do not alter FIFO correctness; they can still change the UCP
protocol selected above UCT.

Default tuning uses the `UCX_OBMM_*` prefix: `3400MBs` bandwidth, `1800ns`
short overhead, and `2us` bcopy overhead. These conservative NC values model
the NC path. `iface_query()` now exposes 112-byte total `max_short` and
65,536-byte `max_bcopy`; UCP combines those hard limits with
`iface_estimate_perf()` to select protocols. The smaller limits and
descriptor-backed receive path can change UCP eager fragmentation and protocol
thresholds, so target protocol-selection logs and benchmarks are required
before drawing performance conclusions.

There are no MD-level region classification knobs. Candidate transport blocks
are discovered by `priv=ucx-obmm:NN` metadata, and memid is never a peer
identity or user-facing allow-list.

UCP protocol-selection logging is intentionally retained. Use
`UCX_PROTO_SELECT_LOG=y` and `UCX_PROTO_SELECT_LOG_RANK=<rank>` to emit
one-shot `ucp_proto_select:` lines for selected UCT lanes without mixing with broad
debug logs.

---

## Diagnostics And Agent Handoff

VFS refresh exposes the current iface geometry and live RX state:

```text
fifo_size
fifo_elem_size
bcopy_seg_size
rx_desc_count
pending_quota
rx/fifo_poll_count
rx/read_index
rx/head
rx/tail
rx/block/fifo_stride
rx/block/fifo_offset
rx/block/length
rx/desc_pool/stride
rx/desc_pool/free
rx/desc_pool/held
```

Use these values to separate configuration errors from runtime stalls. For a
hang, compare `head`, `tail`, and `read_index`; then inspect the expected slot's
OWNER bit. If the expected slot is bcopy and `free=0`, descriptor backpressure
is the first explanation; otherwise inspect the producer that reserved that
absolute index before changing fences or polling. For setup failures, start
from the exact emitted error and
trace controller discovery, strict private-metadata filtering, tuple lookup,
mapping, block open/attach, and geometry validation in that order.

A new agent should read the implementation in this sequence:

1. This document and `obmm-api-and-env/SKILL.md` for accepted contracts.
2. `obmm_md.h`, `obmm_iface.h`, and `obmm_ep.h` for object ownership.
3. `obmm_fifo.h` and `obmm_block.h` for shared FIFO/descriptor layout and
   protocol constants.
4. `obmm_sysfs.c` and `obmm_region.c` for discovery/mapping boundaries.
5. `obmm_block.c`, `obmm_iface.c`, and `obmm_ep.c` for claim, progress, and
   send ordering.
6. `plan.md` only for historical measurements and rejected alternatives.

Do not infer active behavior from old plan entries, probes, benchmark files,
or other transports when this document and current code establish a narrower
contract. Conversely, if current code contradicts an invariant here, treat it
as a design/code inconsistency to resolve explicitly, not as permission to
silently weaken the document.

### Design Change Checklist

Before changing lifecycle, capabilities, address format, layout, or tuning:

1. State the intended behavior in this file before editing code.
2. Check ops-table entries, capability flags, numeric caps, MD flags, and the
   performance callback as one unit.
3. Check device/iface address lengths, reachability, tuple lookup, EP setup,
   self-loop borrowing, and cleanup as one unit.
4. Check shared offsets, field widths, alignment, overflow validation, block
   size, coloring slack, and initialization/READY publication as one unit.
5. Check producer reservation, publish fences, receiver fences, tail reuse,
   pending ordering, and the rank-failure model as one unit.
6. Decide whether the change affects UCP protocol selection even if it appears
   local to UCT.
7. Perform static diff review locally, then build and inspect capabilities on
   Linux; reserve hardware/performance conclusions for the real environment.

---

## Verification

Local Windows verification is limited to static checks. Do not run `mpirun`,
`ucx_perftest`, or two-node hardware tests locally.

Target checks:

1. Build UCX on Linux.
2. `UCX_TLS=obmm ucx_info -d -t obmm` should show one TLS with AM short/bcopy,
   pending, and `INTER_NODE`, with no `am_zcopy`.
3. `ucx_info -c | grep OBMM` should show the shared `OBMM_*` tuning knobs,
   with no memid allow-list, same-node, or `OBMM_CC_*` configuration.
4. Validate NC-only local identity discovery, iface export claiming, and
   ep-time peer block resolution.
5. Validate OSU behavior on the real setup; do not claim target performance
   from local static checks.
