## Current Session Plan

- [x] Revert the failed `fr` HEAD fence-only change back to the previous commit behavior.
- [x] Re-check the AM id 0 report against the bulk descriptor lifecycle.
- [x] Make `seq` the first invalidated field during bulk window reclaim/cleanup.
- [x] Revalidate and snapshot bulk descriptor fields before invoking AM.
- [x] Review the diff with the OBMM/UCT constraints and record what could not be locally tested.

## Current Debug Constraint

- Test logs cannot be copied from the target environment; added logs must be
  short, key-field oriented, and easy to type manually.
- Prefer one diagnostic round only: one-shot path logs plus one stall summary.
