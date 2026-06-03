# Session Plan: NC Medium Path, CC Large Path

## Trigger

The FIFO-only AM path now runs the OSU suite, but large-message performance is
poor because the prior 19 KiB `max_bcopy` forces UCP to split MiB-scale sends
into many NC FIFO publications. The final design should not make NC the large
data path: NC remains the control and medium-message path, while CC memory will
carry large-message data through a later `am_zcopy` or AM rendezvous path.

## Step 1: NC bcopy to 192 KiB

- `UCT_OBMM_POOL_SLOT_COUNT = 96`
- `UCT_OBMM_SHORT_LANE_COUNT = 0`
- `UCT_OBMM_WIRE_FORMAT_VERSION = 4`
- `FIFO_SIZE = 128`
- `FIFO_ELEM_SIZE = 2048`
- `BCOPY_SEG_SIZE = 196608`
- `elem->length = uint16_t` for short; bcopy length is carried in `header`

The shared FIFO remains the only NC AM publication path:

```
flags & BCOPY    -> payload lives in desc[idx]
                    header carries payload length
!(flags & BCOPY) -> FIFO element carries inline am_short [header|payload]
```

Size check:

```
pool_overhead = 2368 bytes
slot_stride   = 128 + 128 * (2048 + 196608)
              = 25428096 bytes
required      = 2368 + 96 * 25428096
              = 2441099584 bytes
              = 2328.014 MiB = 2.273 GiB
```

The target NC region must be at least 2,441,099,584 bytes. Operationally,
allocate 2.5 GiB or 3 GiB rather than running exactly at the boundary.

## Step 2: CC large-message path

Planned direction:

- Add user-provided NC/CC region classification.
- Keep NC FIFO for control messages, small AM, pending, and completion ACKs.
- Add CC-backed `am_zcopy` or AM rendezvous for large payloads.
- Keep CC memory as a bounded shared credit/window pool, not
  `fifo_size * max_zcopy * slot_count`.
- Model UCP performance per operation so `AM_BCOPY` reflects NC and
  `AM_ZCOPY` reflects CC.

Initial target geometry:

```
CC chunk size   = 2 MiB
CC credits/slot = 8 or 16
CC memory       = 1.5 GiB or 3 GiB for 96 slots
```

## Self-Review Checklist

- Keep advertised capabilities to `AM_SHORT` and `AM_BCOPY` in step 1.
- Do not call any libobmm export/import/ownership APIs in the NC path.
- Keep `am_short` and `am_bcopy` using the same FIFO reservation and pending
  backpressure path.
- Keep `BCOPY` as the wire discriminator; absence of `BCOPY` means inline
  FIFO short.
- Keep full bus-fence tail release after invoking AM handlers.
- Validate `BCOPY_SEG_SIZE` against UCP's real AM segment-size propagation:
  64-byte aligned and no larger than `65535 * 64`.
- Keep pool geometry, iface address checks, capabilities, and DESIGN.md in
  sync.
- Local validation is static only in this Windows workspace; Linux build and
  hardware validation remain target-side work.
