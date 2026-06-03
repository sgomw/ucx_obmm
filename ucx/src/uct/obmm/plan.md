# Session Plan: Small NC Path, CC Large Path

## Trigger

The FIFO-only AM path now runs the OSU suite, but large-message performance is
poor over NC. Raising NC `max_bcopy` to 192 KiB increased fixed pool memory by
roughly 6x and only recovered medium-message latency to the old level when
`UCX_RNDV_THRESH` was fixed low. This session rolls NC bcopy back to the
pre-192 KiB geometry and keeps NC as the control/small-message path. CC memory
will carry large-message data through a later `am_zcopy` or AM rendezvous path.

## Step 1: NC bcopy rollback

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
              = 255.764 MiB = 0.250 GiB
```

The target NC region is currently 3 GiB, which is intentionally oversized for
the rollback. Keep metadata-reset-on-exit so stale larger-geometry headers do
not survive between runs, without zeroing the full 3 GiB region.

## Step 2: CC large-message path

Planned direction:

- Add user-provided NC/CC region classification.
- Keep NC FIFO for control messages, small AM, pending, and completion ACKs.
- Add CC-backed `am_zcopy` or AM rendezvous for large payloads.
- Keep CC memory as a bounded shared credit/window pool, not
  `fifo_size * max_zcopy * slot_count`.
- Model UCP performance per operation so `AM_BCOPY` reflects NC and
  `AM_ZCOPY` reflects CC.

## Self-Review Checklist

- Keep advertised capabilities to `AM_SHORT` and `AM_BCOPY` in step 1.
- Do not call any libobmm export/import/ownership APIs in the NC path.
- Keep `am_short` and `am_bcopy` using the same FIFO reservation and pending
  backpressure path.
- Keep `BCOPY` as the wire discriminator; absence of `BCOPY` means inline
  FIFO short.
- Keep full bus-fence tail release after invoking AM handlers.
- Carry `slot_count` and `short_lane_count` in `uct_obmm_iface_addr_t` so
  reachability rejects stale peers before ep creation.
- Keep metadata-reset-on-exit and slot-zero-on-allocation.
- Keep pool geometry, iface address checks, capabilities, and DESIGN.md in
  sync.
- Local validation is static only in this Windows workspace; Linux build and
  hardware validation remain target-side work.
