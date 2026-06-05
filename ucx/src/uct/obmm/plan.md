# Session Plan: Short-First NC, CC Large Path

## Trigger

The latest measurements show that increasing `FIFO_ELEM_SIZE` sharply lowers
latency for message sizes covered by NC inline FIFO short. For obmm, `am_short`
and `am_bcopy` both publish through the same NC FIFO; bcopy adds pack callback,
paired-desc storage, and a receiver-side desc branch without providing a real
hardware benefit for the current NC path. The next goal is to find the
crossover where expanded NC inline short becomes slower than a future CC
staged large-message path.

## Step 1: NC short-first crossover search

Current defaults for target-side testing:

- `UCT_OBMM_POOL_SLOT_COUNT = 96`
- `UCT_OBMM_SHORT_LANE_COUNT = 0`
- `UCT_OBMM_WIRE_FORMAT_INLINE32 = 3`
- `FIFO_SIZE = 64`
- `FIFO_ELEM_SIZE = 520128`
- `BCOPY_SEG_SIZE = 4096`

The shared FIFO remains the only NC AM publication path:

```
flags & BCOPY    -> payload lives in desc[idx]
!(flags & BCOPY) -> FIFO element carries inline am_short [header|payload]
```

`elem->length` is now 32-bit, so inline short is no longer capped near 64 KiB
by software wire format. `BCOPY_SEG_SIZE` is intentionally small: UCP requires
AM bcopy for wireup/control/fallback paths, but bcopy is not a performance
target in this design. UCP's hard wireup floor is 64 B; the 4 KiB default keeps
control-message headroom while removing the old 19 KiB bcopy footprint.

## Size Check

With the current C layout on the target 64-byte cacheline architectures:

```
pool_overhead = 2368 bytes
slot_stride   = 128 + 64 * (520128 + 4096)
              = 33550464 bytes
required      = 2368 + 96 * 33550464
              = 3220846912 bytes
              = 3071.639 MiB = 2.999 GiB
raw_short_cap = 520128 - offsetof(header)
              = 520112 total bytes
max_short     = raw_short_cap in NC-only mode; capped below CC_MIN_ZCOPY
              when CC staged AM_ZCOPY is enabled
max_bcopy     = 4096 bytes
```

This intentionally uses nearly all of the current 3 GiB NC region to maximize
the observable short range. If target attach fails because the exported region
is smaller than exactly 3 GiB, reduce `UCX_OBMM_FIFO_ELEM_SIZE`; 516096 still
covers the 256 KiB OSU size with the same 4 KiB bcopy default and more margin.

## IOV Review

UCT does not have a separate `am_bcopy_iov` operation. The relevant choices are
`ep_am_short_iov` and `ep_am_zcopy`.

For this crossover search, do not implement `ep_am_short_iov` yet:

- MPI tag contiguous sends in the OSU path select ordinary `uct_ep_am_short`.
- `ep_am_short_iov` mainly helps UCP AM sends that carry a separate user
  header and payload.
- Implementing it now would add another variable before the NC/CC crossover is
  measured.

Revisit `ep_am_short_iov` after the crossover is known, especially if UCP AM or
non-tag workloads become a target.

## Step 2: CC large-message path

Chosen direction:

- Implement CC as a staged UCT `am_zcopy` path, not as a larger `am_bcopy` and
  not as PUT/GET/RMA first.
- Keep NC FIFO for control messages, small AM, pending, slot credits, and CC
  completion ACKs.
- Keep `am_bcopy` at the minimum UCP needs for wireup/control/fallback; it is
  not a performance path for this transport.
- Add user-provided NC/CC region classification. NC mappings stay `O_SYNC` and
  must never use ownership changes. CC mappings must be cacheable, opened
  without `O_SYNC`, initially mapped `PROT_NONE`, and all
  `obmm_set_ownership` ranges must be page-aligned.
- Publish `UCT_IFACE_FLAG_AM_ZCOPY` only after the asynchronous CC state machine
  exists. UCP proto-v2 rejects a nonzero AM zcopy `min_zcopy`, so OBMM keeps
  advertised `min_zcopy = 0` and uses `CC_MIN_ZCOPY` as the local NC/CC
  crossover/cap hint. After target probing, the default crossover is 2 MiB,
  not the earlier 192-256 KiB candidate.
- Use receiver-owned staging. Target probing showed sender-owned remote-read
  cost dominates the current path, while receiver-owned reduces the read side
  consistently from 256 KiB through 4 MiB. Each sender reserves one receiver
  CC credit through the receiver's NC `cc_head`, writes the receiver's imported
  CC chunk, releases write ownership, and notifies the receiver. The receiver
  reads its local exported CC chunk, releases read ownership, advances `cc_tail`
  after contiguous CC sequences complete, and ACKs the sender.
- Manage CC payload memory as a bounded per-process credit/window pool. The
  current default geometry is 4 chunks per local process at 4 MiB per chunk,
  which costs 96 * 4 * 4 MiB = 1536 MiB per CC region. Target data shows this
  is a better high-concurrency point than 6 MiB chunks, even if UCP may split a
  4 MiB payload because of protocol headers. Increase chunk count only if
  credit starvation appears; do not size CC as
  `fifo_size * max_zcopy * slot_count`.
- For high-concurrency zcopy regression diagnosis, keep logs aggregate and
  low-noise: enable `UCX_OBMM_CC_DIAG=y` and inspect only `obmm_cc_diag:` lines
  for the active 2 MiB/4 MiB buckets.
- Keep payload and metadata lifetimes separate. The UCT zcopy completion may be
  invoked once the source iovs have been copied into CC and write ownership has
  been released, because the source buffer can then be reused. The CC slot
  itself is not reusable until the receiver sends an NC ACK/credit after its AM
  callback has consumed the payload.
- Large-message UCP policy should be tested in two modes once `am_zcopy` is
  available: eager AM zcopy for the middle range
  (`ZCOPY_THRESH=crossover`, `RNDV_THRESH` above it), and UCP
  `rndv/am/zcopy` at or above the crossover. The UCT data movement primitive is
  the same; UCP's eager-vs-rendezvous protocol decides matching, buffering, and
  round-trip cost.
- UCP AM_ZCOPY protocols in the open-source framework do not declare
  `UCP_PROTO_COMMON_INIT_FLAG_MIN_FRAG`, so OBMM must not advertise a non-zero
  `cap.am.min_zcopy`. Keep `CC_MIN_ZCOPY` as the OBMM crossover used to cap
  `max_short`; advertise `min_zcopy=0` and let `max_zcopy`/chunk size remain
  the hard UCT bound.
- Since `min_zcopy=0` can make UCP call `ep_am_zcopy` for tiny
  rendezvous/control fragments, OBMM internally routes sub-`CC_MIN_ZCOPY`
  zcopy calls that are at least 8 bytes and fit the raw inline FIFO capacity
  back to NC short. These fragments must not take CC ownership.
- The final CC protocol-shape probe is
  `ucx/src/uct/obmm/probes/obmm_cc_batch_path_probe.c`. It tests a
  receiver-owned bulk epoch: one sender WRITE ownership transition covers
  multiple contiguous messages, then one receiver READ ownership transition
  consumes the same batch. Treat `batch=1` as the current per-message
  receiver-owned baseline. If `per_msg_us` and ownership-per-message do not
  fall substantially for `batch=2/4/8`, a batch/epoch CC transport will not
  fix the high-concurrency large-message regression and CC should remain
  conservative or optional.

## Step 3: UCP cost-model alignment

The default UCP thresholds should not be required for the final shape. OBMM
must report operation-specific performance so UCP can naturally prefer:

- NC FIFO `AM_SHORT` below the configured crossover.
- Tiny `AM_BCOPY` only for wireup/control/fallback and multi-frag fallback.
- CC staged `AM_ZCOPY` for large rendezvous data once CC is enabled.

`CC_MIN_ZCOPY` still caps advertised `max_short` when CC is enabled. This cap
is necessary because UCP has no multi-fragment short protocol: if raw
`max_short` remains the full FIFO capacity, a whole message below that capacity
can be consumed by single-fragment `eager/short` before `AM_ZCOPY` is even a
candidate. The cap is not a performance estimate; it is a capability boundary
that lets messages at and above the NC/CC crossover leave the short path.

The performance model must distinguish UCT operations:

- `AM_SHORT`: NC FIFO, low per-side overhead, NC bandwidth.
- `AM_BCOPY`: NC FIFO plus pack/desc branch, higher per-side overhead, NC
  bandwidth, and small `max_bcopy` so UCP accounts for many fragments.
- `AM_ZCOPY`: CC staged path, high per-side ownership/staging overhead and
  staged-copy bandwidth. Defaults are conservative model hints calibrated from
  the current ownership probe, not raw CC hardware bandwidth.
- Use `ucx/src/uct/obmm/probes/obmm_cc_ownership_probe.c` to calibrate the
  real CC ownership cost. `obmm_set_ownership()` accepts PAGE_SIZE-aligned
  ranges, but the effective writeback/invalidate granularity may still be the
  OBMM base granule, typically PMD/2 MiB. If so, `CC_CHUNK_SIZE` must be at
  least that granule, each CC slot/chunk base must be granule-aligned, and the
  UCP model must charge per-granule ownership cost, not per requested byte.
  Current target probing reports `PMD_2M_LIKELY`, so `CC_OWN_GRANULE` defaults
  to 2 MiB. The data path flips `align_up(header + payload, CC_OWN_GRANULE)`
  bytes rather than the whole chunk.

Receiver-owned CC zcopy state:

1. A receiver CC chunk starts free with no readable or writable ownership.
2. The sender reserves a receiver chunk by CAS-incrementing the receiver NC
   `cc_head`; the chunk id is `receiver_cc_seq % CC_CHUNK_COUNT`.
3. The sender raises write ownership on the peer/imported CC mapping for the
   receiver chunk.
4. The sender copies AM header and payload iovs into the receiver CC chunk.
5. The sender drops ownership back to none, triggering the required writeback.
6. The sender sends or internally queues an NC `CC_DATA_READY` control record
   containing sender identity, sender slot/generation, sender sequence,
   receiver CC sequence, chunk id, and length.
7. The receiver raises read ownership on its local/exported CC chunk, invokes
   the AM callback synchronously with the payload pointer, drops ownership back
   to none, marks the receiver CC sequence complete, advances `cc_tail` for
   contiguous completions, and sends an NC ACK to the sender's FIFO.
8. The sender completes its outstanding zcopy bookkeeping when it receives the
   ACK. The receiver-owned chunk is reusable once `cc_tail` advances, not when
   the sender sees the ACK.

Correctness risks to handle in the implementation:

- `ep_am_zcopy` is asynchronous and must support `UCS_INPROGRESS`,
  `UCS_ERR_NO_RESOURCE`, completions, pending retry, and flush progress.
- `iface_flush`/`ep_flush` must account for outstanding CC zcopy operations;
  the current AM-only flush behavior is not enough once zcopy can be
  in-progress.
- CC chunk ranges must not overlap between local processes. The OBMM ownership
  model is page/range based and shared mappings on the same page can suppress
  writeback/invalidation until the last process releases permission.
- If target probing shows PMD-sized effective ownership, adjacent chunks below
  that size can share one ownership granule. Treat sub-granule chunk sizes as
  unsafe for concurrent staged zcopy. The current implementation rejects
  `CC_CHUNK_SIZE < CC_OWN_GRANULE` or chunk sizes that are not granule
  multiples, and it maps CC regions on a 2 MiB-aligned virtual base.
- Receiver AM data is callback-lifetime only. Releasing the CC chunk after the
  synchronous callback returns is valid; retaining the pointer beyond callback
  return is not.
- Crash/reset handling must return owned CC chunks to no-access state and
  recycle credits without corrupting another live process's slot.

## Self-Review Checklist

- Keep advertised capabilities to `AM_SHORT` and `AM_BCOPY` in step 1.
- Do not call any libobmm export/import/ownership APIs in the NC path.
- Keep `am_short` and `am_bcopy` using the same FIFO reservation and pending
  backpressure path.
- Keep `BCOPY` as the wire discriminator; absence of `BCOPY` means inline
  FIFO short.
- Keep full bus-fence tail release after invoking AM handlers.
- Carry wire format, slot count, and short lane count in
  `uct_obmm_iface_addr_t` so reachability rejects stale peers before ep
  creation.
- Keep metadata-reset-on-exit and slot-zero-on-allocation.
- Keep pool geometry, iface address checks, capabilities, and DESIGN.md in
  sync.
- Local validation is static only in this Windows workspace; Linux build and
  hardware validation remain target-side work.
