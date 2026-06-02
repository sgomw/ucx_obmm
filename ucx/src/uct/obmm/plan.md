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
