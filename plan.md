## Current Session Plan

- [x] Revert the failed `fr` HEAD fence-only change back to the previous commit behavior.
- [x] Re-check the AM id 0 report against the bulk descriptor lifecycle.
- [x] Make `seq` the first invalidated field during bulk window reclaim/cleanup.
- [x] Revalidate and snapshot bulk descriptor fields before invoking AM.
- [x] Interpret 32K stall logs: bulk control catches up, but `rxB` can exceed
  `txB`, pointing to duplicate descriptor delivery across multiple EPs.
- [x] Check the CC atomic/ownership hypothesis: current NC eager/control atomics
  are on NC control memory; bulk ownership flips are limited to CC data windows.
- [x] Claim a bulk descriptor with `ack_generation` before invoking AM so only
  one EP delivers it; sender reclaim still waits for `ack_seq == seq`.
- [x] Keep stall diagnostics to one summary per iface lifetime.
- [x] Review the diff with the OBMM/UCT constraints and record what could not be locally tested.

## Current Debug Constraint

- Test logs cannot be copied from the target environment; added logs must be
  short, key-field oriented, and easy to type manually.
- Prefer one diagnostic round only: one-shot path logs plus one stall summary.
