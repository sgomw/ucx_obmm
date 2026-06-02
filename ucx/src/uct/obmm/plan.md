# Session Plan: 96 Slots Per Node

## Goal

Raise the current NC AM-only obmm transport from 32 local process slots per
node to 96 slots per node after the deployed NC export/import regions were
raised to 256 MiB.

## Current Geometry Decision

- `UCT_OBMM_POOL_SLOT_COUNT = 96`
- `UCT_OBMM_SHORT_LANE_GROUP_COUNT = 2`
- `UCT_OBMM_SHORT_LANE_COUNT = 192`
- `UCT_OBMM_SHORT_LANE_BITMAP_WORDS = 3`
- Keep latency/performance geometry unchanged:
  - `FIFO_SIZE = 64`
  - `FIFO_ELEM_SIZE = 64`
  - `BCOPY_SEG_SIZE = 32768`
  - short lane FIFO depth = 8
  - short lane element size = 256

The short-lane count must scale with slot count because sender lane selection
uses:

```
lane_index = local sender slot_index
lane_index += slot_count for import-side senders
```

Keeping only 64 lanes would break deterministic SPSC short lanes once
`slot_index >= 64`, or force the transport back toward shared-lane arbitration.
The 192-lane layout preserves the current no-CAS `am_short` fast path.

## Size Check

With the current C layout on the target 64-byte cacheline architectures:

```
pool_overhead = 2368 bytes
slot_stride   = 2543808 bytes
required      = 2368 + 96 * 2543808
              = 244207936 bytes
              = 232.895 MiB
```

This fits in a 256 MiB region with about 23.1 MiB of headroom.

## Self-Review Checklist

- Do not change advertised UCT capabilities.
- Do not change FIFO size, element stride, bcopy segment size, short FIFO
  depth, or short element size.
- Keep pool slot count and short lane count derived from the same header so
  ep lane selection and slot allocation cannot diverge.
- Carry `slot_count` and `short_lane_count` in `uct_obmm_iface_addr_t` so
  reachability rejects stale peers before ep creation.
- Expand the short-lane active bitmap from one 64-bit word to enough words for
  all lanes.
- Update workflow docs that lag the current 256 MiB / 96-slot environment.
- Local validation is static only in this Windows workspace; Linux build and
  hardware validation remain target-side work.
