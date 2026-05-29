## Current Session Plan

- [x] Revert the failed `fr` HEAD fence-only change back to the previous commit behavior.
- [x] Re-check the AM id 0 report against the bulk descriptor lifecycle.
- [x] Make `seq` the first invalidated field during bulk window reclaim/cleanup.
- [x] Revalidate and snapshot bulk descriptor fields before invoking AM.
- [x] Review the diff with the OBMM/UCT constraints and record what could not be locally tested.
