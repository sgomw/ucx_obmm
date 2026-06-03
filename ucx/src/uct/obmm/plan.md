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
max_short     = 520128 - offsetof(header)
              = 520112 total bytes
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

Planned direction:

- Add user-provided NC/CC region classification.
- Keep NC FIFO for control messages, small AM, pending, and completion ACKs.
- Add a CC staged `am_zcopy` / rendezvous path for payloads above the measured
  crossover.
- Manage CC memory as a bounded credit/window pool, not
  `fifo_size * max_zcopy * slot_count`.
- Model UCP performance per operation so `AM_SHORT` reflects NC inline and
  `AM_ZCOPY` reflects CC staged transfer.

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
