<overview>
Implementing v2 of the obmm UCT transport's am_bcopy operation: turning the v1 degenerate path (bcopy used FIFO element body, max_bcopy ≈ 2032) into a real bulk-copy path with an independent per-FIFO-elem desc area (max_bcopy = configurable seg_size). Approach: extend each pool slot with a 1:1 paired desc array of size `fifo_size * bcopy_seg_size`, so desc lifetime ≡ FIFO elem lifetime — no separate desc free-list, no extra cross-host CAS. Constraints: stay inside `ucx/src/uct/obmm/`; ARM64 production; cross-host NC mapping with bus-domain fences.
</overview>

<history>
1. User asked to (a) write the v2 design somewhere I'll see during dev, (b) implement complete am_bcopy (mm-style independent area), (c) skip tests for now.
   - Re-read existing data path: `obmm_iface.c` (query/progress/init), `obmm_ep.c` (am_short, reserve_slot, am_bcopy, pending_add), `obmm_fifo.h`, `obmm_iface.h`, `obmm_pool.h`, and `mm_iface.c` cap layout for reference.
   - Discovered mm uses a separately-attached SHM segment (`seg_id` per desc) — not directly portable to obmm because we have only the single 128 MiB pre-exported region. Decided on 1:1 FIFO-elem ↔ desc pairing inside the same slot.

2. Wrote `ucx/src/uct/obmm/DESIGN.md` capturing v2 layout, send/recv sequences, capability table, configuration knobs, and future scope (zcopy/RMA out of scope).

3. Ran the rubber-duck agent on the design (per AGENTS.md mandatory workflow). It returned two BLOCKERS and several non-blocking findings:
   - **BLOCKER #1**: `ucs_memory_bus_store_fence()` (= `dmb oshst` on ARM64) is store→store only; cannot order receiver's prior LOADS from `desc[N]` (inside AM handler) before the STORE that publishes `tail`. Need a load→store / full bus fence.
   - **BLOCKER #2**: slot stride must be computed/validated as `size_t` BEFORE narrowing to `uint32_t` for `pool_attach`, otherwise large `BCOPY_SEG_SIZE` could overflow/truncate and produce overlapping pointers.
   - Other accepted findings: bump `UCT_OBMM_POOL_VERSION` (slot semantics changed even though header layout didn't); better is_reachable_v2 diagnostic with both peer & local geometry; document ephemeral callback semantics; add post-pack assert; fail clearly at attach (don't silently cap seg_size).
   - Confirmed: keep `uint16_t length`, keep flags=0 (no `UCT_CB_PARAM_FLAG_DESC`).

4. Surveyed existing fence helpers in `ucx/src/ucs/arch/`: only ppc64 has `ucs_memory_bus_fence()`. Decided to define `uct_obmm_bus_full_fence()` locally in `obmm_fifo.h` with arch ifdefs (aarch64 → `dmb osh`, x86 → `mfence`, ppc64 → `sync`, rv64 → `fence iorw,iorw`, fallback → load+store fence with `#warning`). This avoids modifying ucs/arch (outside the obmm directory per AGENTS.md hard rule).

5. Implemented the v2 changes across the obmm transport files (see Work Done).
</history>

<work_done>
Files updated:
- `ucx/src/uct/obmm/DESIGN.md` (NEW): Full v2 design doc — region/slot layout, FIFO elem layout, sender/receiver sequences, capability table, config knobs, version-bump rationale, future scope, no-hardware verification.
- `ucx/src/uct/obmm/base/obmm_pool.h`: Bumped `UCT_OBMM_POOL_VERSION` from 1 → 2 (so v1 binaries cannot attach to v2-initialized regions, and vice versa).
- `ucx/src/uct/obmm/base/obmm_fifo.h`:
  - Added `bcopy_seg_size` parameter to `uct_obmm_slot_stride()` (now sums `ctl + fifo_size*elem_size + fifo_size*seg_size`).
  - Added `uct_obmm_slot_descs(slot_base, fifo_size, fifo_elem_size)` helper.
  - Added `uct_obmm_slot_desc(descs, index, mask, seg_size)` helper.
  - Added `uct_obmm_bus_full_fence()` macro with per-arch asm (aarch64/x86/ppc64/rv64/fallback).
- `ucx/src/uct/obmm/base/obmm_iface.h`:
  - Replaced `reserved` u32 with `bcopy_seg_size` in `uct_obmm_iface_addr_t` (struct size unchanged).
  - Added `bcopy_seg_size` to `uct_obmm_iface_config_t`.
  - Added `recv_descs` pointer and `bcopy_seg_size` field to `uct_obmm_iface_t`.
- `ucx/src/uct/obmm/base/obmm_iface.c`:
  - New `BCOPY_SEG_SIZE` config field, default 4096, doc-string explains region budget tradeoff.
  - `iface_query`: `max_bcopy = bcopy_seg_size` (no longer == max_short).
  - `iface_get_address`: writes `iaddr->bcopy_seg_size`.
  - `is_reachable_v2`: now also checks `bcopy_seg_size` match; richer info string with both peer & local fifo/elem/seg.
  - Init: validate `BCOPY_SEG_SIZE > 0` and `≤ UINT16_MAX`; compute `stride` as `size_t`; check `stride ≤ UINT32_MAX`; check `pool_required_size ≤ region->length` with detailed error pointing at all geometry knobs; populate `recv_descs` after pool slot alloc.
  - `iface_progress`: bcopy dispatch reads from `desc[N]` instead of `(elem+1)`; replaced `ucs_memory_bus_store_fence()` with `uct_obmm_bus_full_fence()` before `tail` publish (the BLOCKER #1 fix).
- `ucx/src/uct/obmm/base/obmm_ep.h`: added `peer_descs` pointer and `bcopy_seg_size` field to `uct_obmm_ep_t`.
- `ucx/src/uct/obmm/base/obmm_ep.c`:
  - ep init: geometry check now includes `bcopy_seg_size`; pool `slot_size` validation passes `bcopy_seg_size` to `uct_obmm_slot_stride`; caches `peer_descs` and `bcopy_seg_size`.
  - `uct_obmm_ep_am_bcopy` rewritten: `pack_cb` writes into `desc[N]` (computed via `uct_obmm_slot_desc`); asserts `length ≤ bcopy_seg_size` and `≤ UINT16_MAX`; bus_store_fence before publishing flags (release barrier orders desc writes + elem hdr writes before flags publish).

Work completed:
- [x] Re-read all data-path files
- [x] Wrote DESIGN.md
- [x] Rubber-ducked the design; adopted blockers + non-blocking findings
- [x] Bumped pool version
- [x] Added desc-area helpers + full bus fence helper in obmm_fifo.h
- [x] Extended config + iface struct + iface_addr in obmm_iface.h
- [x] Added BCOPY_SEG_SIZE config, query, get_address, is_reachable_v2 updates in obmm_iface.c
- [x] Updated iface init: stride validation, region-budget check, recv_descs setup
- [x] Updated iface_progress: bcopy via desc[N], full bus fence before tail
- [x] Extended obmm_ep_t fields
- [x] Updated ep init: geometry check incl. seg_size, peer_descs caching
- [x] Rewrote am_bcopy to use desc[N]
- [ ] Build verification (NOT YET RUN)
- [ ] code-review of diff (NOT YET RUN)
- [ ] User-driven cross-node test on real hardware (deferred)
</work_done>

<technical_details>
- **Design choice — 1:1 desc/elem pairing**: instead of mm's separately-attached SHM segment with its own free-list, each FIFO slot now contains `fifo_size * bcopy_seg_size` bytes of desc area immediately after the FIFO element array. Slot N's bcopy desc is at index `N & mask` in that area. Sender's existing FIFO head reservation (load+CAS on peer head) implicitly reserves the matching desc; receiver's tail bump implicitly releases both. No separate desc allocator, no per-desc CAS.

- **BLOCKER fix — full bus fence on tail release**: on ARM64 NC mappings, `ucs_memory_bus_store_fence()` = `dmb oshst` orders only store→store. The receiver's AM handler issues LOADS from `desc[N]`, then bumps tail with a STORE — load→store ordering is required. Defined `uct_obmm_bus_full_fence()` locally with per-arch asm (aarch64: `dmb osh`, x86: `mfence`, ppc64: `sync`, rv64: `fence iorw,iorw`). Located in `obmm_fifo.h` to keep the change inside `ucx/src/uct/obmm/` per AGENTS.md hard rules. Could later be promoted to ucs/arch with user approval.

- **Stride overflow validation**: must compute `uct_obmm_slot_stride()` as `size_t`, check `≤ UINT32_MAX` (pool_hdr->slot_size is u32) AND check `pool_required_size ≤ region->length`, BEFORE the cast and pool_attach call. Done in `iface init`. Errors include all geometry knobs to make UX clear.

- **Pool version bump rationale**: header layout unchanged but per-slot interpretation changed (slot now contains desc area). slot_size mismatch already rejects, but explicit version bump produces clearer "wire/layout version mismatch" diagnostic and avoids hypothetical aliased slot_size collisions.

- **Default geometry**: `FIFO_SIZE=64, FIFO_ELEM_SIZE=2048, BCOPY_SEG_SIZE=4096`. Per-slot ≈ 384 KiB. 256 slots ≈ 96 MiB ≤ 128 MiB ✓. If user bumps `BCOPY_SEG_SIZE` to 8192, total grows to ~160 MiB and attach fails with descriptive error.

- **`elem->length` stays `uint16_t`** → `BCOPY_SEG_SIZE ≤ UINT16_MAX = 65535`. Validated at init. Widening would require pool version bump and is not justified yet.

- **`is_reachable_v2` checks all three geometry fields** (`fifo_size, fifo_elem_size, bcopy_seg_size`). Heterogeneous `UCX_OBMM_BCOPY_SEG_SIZE` settings within a job will silently lose endpoints — but auto-capping would cause UCP to lie about max_bcopy, which is worse.

- **Ephemeral callback semantics**: bcopy receive calls `uct_iface_invoke_am(..., desc, length, 0)` — flags=0 means the desc pointer is callback-ephemeral; UCX upper layers must copy out. This is consistent with no separate desc free-list. Documented in DESIGN.md.

- **Open / unverified items**:
  - Build NOT YET attempted — possible compile errors I haven't caught.
  - No way to test the new ordering on real hardware from this workspace.
  - Pending arbiter still returns `UCS_ERR_BUSY` (UCP retry); could spin under sustained bcopy load — flagged as known limitation.
  - The `#warning` fallback for unknown arch may trip a build with `-Werror`; should be fine on ARM64/x86 dev box.
</technical_details>

<important_files>
- `ucx/src/uct/obmm/DESIGN.md` (NEW)
  - Single source of truth for v2 wire format and data-path semantics; AGENTS.md mandates retrieve-before-recall, this file is the first thing to grep.
- `ucx/src/uct/obmm/base/obmm_fifo.h`
  - Holds the wire layout, slot helpers, and the new `uct_obmm_bus_full_fence()` macro (load→store barrier for cross-host NC).
  - `uct_obmm_slot_stride(fifo_size, elem_size, seg_size)` signature changed — ALL callers updated.
- `ucx/src/uct/obmm/base/obmm_iface.h`
  - `uct_obmm_iface_addr_t` v2 layout (replaced `reserved` u32 with `bcopy_seg_size`); `uct_obmm_iface_t` gained `recv_descs` and `bcopy_seg_size`.
- `ucx/src/uct/obmm/base/obmm_iface.c`
  - All four touchpoints (query/get_address/is_reachable_v2/init/progress) updated. Tail-release barrier replaced with full bus fence (~line 250). bcopy dispatch in progress (~line 230). New BCOPY_SEG_SIZE config (~line 46-54). Stride/budget validation in init (~line 280-310).
- `ucx/src/uct/obmm/base/obmm_ep.h`
  - `uct_obmm_ep_t` gained `peer_descs` and `bcopy_seg_size`.
- `ucx/src/uct/obmm/base/obmm_ep.c`
  - ep init validates `bcopy_seg_size` and computes peer pool stride correctly. `uct_obmm_ep_am_bcopy` rewritten (~lines 257-330) to write into `desc[N]` and assert length bounds.
- `ucx/src/uct/obmm/base/obmm_pool.h`
  - `UCT_OBMM_POOL_VERSION` bumped to 2.
- `.github/skills/obmm-api-and-env/SKILL.md`
  - Locked-in environment constraints — re-read before any layout change.
- `.github/skills/ucx-build-verify/SKILL.md`
  - Build commands + no-hardware verification checklist for the next step.
- `AGENTS.md`
  - Mandatory workflow; hard rule: don't modify outside `ucx/src/uct/obmm/` without asking the user (drove the choice to put `uct_obmm_bus_full_fence` locally instead of in ucs/arch).
- Reference: `ucx/src/uct/sm/mm/base/mm_ep.c` lines 277-360 (`uct_mm_ep_am_common_send`) and `mm_iface.c` lines 198-200 (cap layout).
</important_files>

<next_steps>
Immediate next steps:
1. **Build verify** — invoke the `task` agent with: `cd ucx && ./autogen.sh && ./contrib/configure-devel --prefix=$PWD/install && make -j && make install`. Address any compile errors (most likely candidates: missing include for `ucs_assertv`, `ucs_aarch64_dmb` macro missing if non-aarch64 arch, signature mismatches if I missed a `uct_obmm_slot_stride` caller).
2. After successful build, run no-hardware verification per `.github/skills/ucx-build-verify/SKILL.md`:
   - `./install/bin/ucx_info -d -t obmm` — confirm `am_bcopy: 4096` (not 2032)
   - `./install/bin/ucx_info -c | grep -i OBMM` — confirm `BCOPY_SEG_SIZE` appears
   - `nm -D ./install/lib/libuct.so | grep uct_obmm_ep_am_bcopy` — symbol exists
3. **code-review agent** on the full diff (`git diff ucx/src/uct/obmm/`) before declaring done.
4. Tell user: build/static checks pass, ready to deploy and run the existing `run_mpi_tests.sh` cross-node — which should still pass (sizes ≤ 1900 fit comfortably in v2 max_bcopy=4096). Larger-size tests (>2032, >4096) are a separate test-extension task the user explicitly deferred.

Open question to flag once user is back: whether to also widen the `uct_obmm_bus_full_fence` to ucs/arch for cleanliness (would require approval to edit outside obmm dir).
</next_steps>