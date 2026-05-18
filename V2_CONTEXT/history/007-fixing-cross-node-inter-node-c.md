<overview>
Implementing the `obmm` UCT transport in UCX (`ucx/src/uct/obmm/`) for cross-host shared memory. v1 (am_short + minimal am_bcopy + pending) is complete and MPI tests pass cross-node. Current segment: cross-node MPI bringup was unblocked by adding `UCT_IFACE_FLAG_INTER_NODE`; user now wants to (a) record the v2 design (full am_bcopy with independent desc pool, mm-style) somewhere persistent and (b) implement that v2 am_bcopy. No tests in this round.
</overview>

<history>
1. User reported single-node MPI sanity passes but cross-node MPI_Init fails: `pml_ucx.c:431 ucp_ep_create failed: Destination is unreachable / select.c:644 no active messages transport: Unsupport operation`.
   - I made many wrong guesses (AM_SYNC missing, version skew, is_reachable_v2, etc.) before grepping the literal error string.
   - User pushed back firmly that single-node passing already implies obmm AM_BCOPY works locally.

2. User requested I focus and stop guessing.
   - I traced "Unsupported operation" string → `select.c:479` (`UCS_STATIC_BITMAP_IS_ZERO(addr_index_map)` branch) → means **no remote address entries** matched the AM lane criteria.
   - Searched `INTER_NODE` → found `ucp_worker.c:2962-2969` filters tl_bitmap by `UCT_IFACE_FLAG_INTER_NODE` when `UCP_WORKER_ADDRESS_FLAG_NET_ONLY` is set.
   - Searched OMPI → `pml_ucx.c:160-161` uses `NET_ONLY` for `PMIX_REMOTE` peers.
   - Root cause: obmm didn't advertise `INTER_NODE`, so OMPI stripped it from cross-node addresses.
   - Fix: added `UCT_IFACE_FLAG_INTER_NODE` to `obmm_iface.c:71-76` cap.flags.
   - Stored memory facts about both the bug and the debugging anti-pattern.

3. User confirmed cross-node MPI tests now all pass and asked me to reflect on why I wasted so much time guessing. I acknowledged I violated AGENTS.md's "Retrieve, do not recall" rule.

4. User asked four design questions:
   - Q1: Why -np=2? Will 40 / odd 39 work?
   - Q2: Are existing tests really only exercising am_short?
   - Q3: Is max_short = max_bcopy = 2032 reasonable?
   - Q4: With short+bcopy, can we run all MPI?
   - I answered each with code citations: pool slots support 256/host, tests do incidentally hit bcopy (sizes up to 1900 → UCP threshold), max equality is the v1 shortcut (mm uses independent desc pool with seg_size for bcopy), and all two-sided MPI / collectives / common benchmarks (OSU, IMB MPI1, NPB, HPL, HPCG) work; only RMA/one-sided fails cross-node.

5. User asked me to (a) document the v2 design somewhere I'll see during dev, (b) implement complete am_bcopy with independent desc pool (mm-style), (c) skip tests for now.
   - I started by re-reading `.github/skills/obmm-api-and-env/SKILL.md` to ensure no contradiction with the v2 plan.
   - **Conversation compacted before I wrote the design doc or began implementation.**
</history>

<work_done>
Files updated this segment (cross-node bringup):
- `ucx/src/uct/obmm/base/obmm_iface.c:71-76`: Added `UCT_IFACE_FLAG_INTER_NODE` to iface_query cap.flags. This single-line change unblocked all cross-node MPI tests.

Work completed:
- [x] Diagnosed cross-node MPI_Init "Unsupported operation" error via literal-string grep
- [x] Added INTER_NODE cap, user confirmed all cross-node MPI sanity/correctness/pingpong/bw/collective tests now pass
- [x] Stored memory facts for: (a) INTER_NODE requirement for cross-host UCT transports, (b) "grep error string first" debugging discipline, (c) cap-flag silent-filter awareness
- [x] Discussed v2 scope with user (4 questions) — design direction agreed
- [x] Re-read obmm-api-and-env SKILL.md (locked-in design constraints still hold)
- [ ] Write v2 design document (not yet started)
- [ ] Implement complete am_bcopy with independent desc pool (not yet started)
</work_done>

<technical_details>

**Root cause of cross-node MPI bringup failure (now fixed)**:
- OMPI `pml_ucx.c:160` calls `mca_pml_ucx_send_worker_address_type(UCP_WORKER_ADDRESS_FLAG_NET_ONLY, PMIX_REMOTE)` to publish the worker address used by **cross-node peers**.
- UCX `ucp_worker.c:2962-2969`: when NET_ONLY is set, only TLs with `UCT_IFACE_FLAG_INTER_NODE` survive into the packed address.
- Single-node uses `PMIX_LOCAL` with flags=0 (no filter), which is why single-node always worked.
- Failure manifests at `select.c:478-481`: when `addr_index_map` is empty after iterating remote address entries, the entire tls_info string becomes literally "Unsupported operation" — there's no per-TL detail.

**Debugging discipline lesson (stored as memory)**:
- For UCX wireup errors: grep the literal error string in `ucx/src` first → find emit site → trace back precondition. Do not enumerate hypotheses from memory. AGENTS.md:6 explicitly warns about "silent capability mismatches" and AGENTS.md:30 says "Retrieve, do not recall."

**v2 design (agreed but NOT yet documented or implemented)**:

Goal: complete am_bcopy that's not capped by FIFO element size.

Reference: mm transport (`ucx/src/uct/sm/mm/base/mm_iface.c:198-200`):
- `max_short = fifo_elem_size - elem_hdr` (FIFO element direct)
- `max_bcopy = config.seg_size` (independent desc pool, default 8K-64K)

Current obmm v1 shortcut: bcopy writes into FIFO element body, so `max_bcopy = max_short = 2032`. This forces UCP to fragment any message > 2032 into many slots, hurting throughput and not exercising the "real" bcopy path.

Proposed v2 layout in the 128 MiB exported region:
- Header (existing): state/version/slot bitmap/slot_meta
- FIFO area (existing): per-iface slots, each with small fixed-size FIFO elements (~2 KB)
- **NEW desc area**: pool of large segments (seg_size, configurable, default 8K-64K)
- Bcopy flow: sender allocates a desc → pack_cb writes into desc → publishes FIFO element with desc index/offset → receiver reads from desc, then frees desc back to pool.

Constraints from `.github/skills/obmm-api-and-env/SKILL.md` that v2 must respect:
- NC mapping (O_SYNC) for the data path — no cacheable mappings on FIFO/desc
- Use bus-domain fences (`ucs_memory_bus_store_fence` / `ucs_memory_bus_load_fence`), NOT cpu-domain
- Cross-node atomic FAA/CAS on NC is supported (per project owner)
- UCT owns in-region layout; two-phase init with state UNINIT→INITING→READY (CAS); never single-magic init
- Reachability key is `(exporter_dcna, exporter_deid, memid)` — not memid alone
- Generation tokens for slot lifecycle to handle PID reuse

**Open design questions for v2 (need to think through before coding)**:
- Desc pool free-list mechanism cross-host (each iface has its own desc area? or a shared pool? mm uses a per-iface descriptor area with sender-allocated/receiver-acked indices)
- Backpressure when desc pool is full — pending_add already returns BUSY, may need same path
- How desc area sizing interacts with fifo_size and 128 MiB region budget; mm uses `seg_size * num_segs` per iface
- Should we keep am_short writing FIFO body and only redirect bcopy to desc area? (yes, for clarity and to keep short fast-path inline)

**Existing test status**:
- All run_mpi_tests.sh tests (sanity, correctness, pingpong, bw, collective) pass cross-node
- Test message sizes capped at 1900 bytes — incidentally exercise UCP's bcopy path above ~200B threshold but not above 2032 (no fragmentation tested)
- After v2, tests should sweep small / medium / large (>2032) sizes explicitly

**Where to put v2 design doc** (user said "wherever you'll see it"):
- Best fit: new `ucx/src/uct/obmm/DESIGN.md` so it sits alongside the code I edit
- Could also extend `.github/skills/obmm-api-and-env/SKILL.md` "Locked-in design decisions" section
- AGENTS.md is too high-level for design specifics
</technical_details>

<important_files>

- `ucx/src/uct/obmm/base/obmm_iface.c`
  - Receive path + iface caps. Center of v2 work.
  - **Lines 71-76**: cap.flags now includes AM_SHORT|AM_BCOPY|PENDING|CONNECT_TO_IFACE|CB_SYNC|INTER_NODE
  - **Lines 84-85**: `max_short = max_bcopy = fifo_elem_size - elem_hdr` (= 2032). v2 must split max_bcopy off to desc seg_size.
  - **Lines 197-250**: iface_progress dispatches BCOPY-flag vs short. v2 needs to also dispatch when BCOPY flag indicates desc-pool offset rather than inline payload.
  - **Lines 259-343**: iface_init wires the FIFO pool. v2 needs to also attach a desc pool.

- `ucx/src/uct/obmm/base/obmm_ep.c`
  - Sender side: `uct_obmm_ep_am_short`, `uct_obmm_ep_am_bcopy`, `uct_obmm_ep_pending_add`, `uct_obmm_ep_reserve_slot`.
  - **am_bcopy (~line 270-310)**: currently `pack_cb(elem+1, arg)` writes into FIFO element body. v2 will: allocate desc → `pack_cb(desc, arg)` → write desc index into elem.
  - pending_add returns UCS_ERR_BUSY (UCP retry); same path will handle desc-pool exhaustion.

- `ucx/src/uct/obmm/base/obmm_pool.h` and `obmm_pool.c`
  - Slot pool implementation (`UCT_OBMM_POOL_SLOT_COUNT = 256` ifaces/host).
  - v2 may extend with a parallel desc pool, or factor a generic "in-region pool" abstraction.

- `ucx/src/uct/obmm/base/obmm_iface.h`
  - Defines `uct_obmm_iface_config_t` (inherits sm config + fifo_size/fifo_elem_size/fifo_max_poll).
  - v2 needs new config fields: `seg_size`, `num_segs` (or compute from region budget).
  - `UCT_OBMM_POOL_SLOT_COUNT = 256` constant lives here.

- `ucx/src/uct/obmm/base/obmm_fifo.h`
  - Element layout (16-byte elem_hdr): `flags(1) + am_id(1) + length(2) + generation(4) + header(8)`. Payload immediately follows at `elem+1`.
  - Has `FLAG_OWNER = UCS_BIT(0)` and `FLAG_BCOPY = UCS_BIT(1)`.
  - v2 may need a third flag `FLAG_DESC` (or repurpose BCOPY to mean "desc-pool indirected") and add a desc-index/offset field, possibly stored in `header` field.

- `ucx/src/uct/sm/mm/base/mm_iface.c`
  - Reference implementation. Lines 183-200 show the cap layout v2 should mirror.
  - mm_ep.c has the bcopy → desc dispatch pattern to study.

- `.github/skills/obmm-api-and-env/SKILL.md`
  - Locked-in design constraints (NC mapping, bus fences, no obmm_set_ownership, layout decisions). v2 design doc must not contradict any of these.

- `AGENTS.md`
  - Mandatory workflow. Specifically: retrieve before recall, plan + rubber-duck before implementing, build verify, code-review.

- `run_mpi_tests.sh`, `mpi_*.c`
  - All passing cross-node now. v2 will need test extensions (post-v2, separately) for >2032 sizes and explicit short/bcopy paths.
</important_files>

<next_steps>

User's explicit ask (in order):
1. Document the v2 design somewhere I'll see during dev (no specific location — my choice).
2. Implement complete am_bcopy (independent desc pool, mm-style).
3. Do NOT write tests yet.

Immediate next steps:

1. **Write design doc** at `ucx/src/uct/obmm/DESIGN.md`:
   - Region layout: header / FIFO area / desc area
   - Sizing: total = 128 MiB; FIFO area sized as today (256 slots × ~128 KB ≈ 32 MiB); desc area gets the remainder (~96 MiB)
   - Desc pool: per-iface or shared? Recommend **per-iface** (matches mm, simpler ownership), each iface gets `num_segs * seg_size` from desc area
   - Free list: in-region SPMC bitmap with CAS allocation, similar to slot pool
   - Default seg_size: 8 KiB; default num_segs per iface: enough to cover FIFO depth (= fifo_size, so each in-flight elem can carry one desc)
   - Wire format: when FLAG_BCOPY set, elem->header carries desc-index; receiver reads from `desc_area + index*seg_size`
   - Receiver desc free: in-place — once invoke_am returns (CB_SYNC), the desc is reusable; sender must wait for FIFO slot ownership flip (which the receiver writes) to know desc is free → simpler: tie desc lifetime to slot lifetime (1:1 mapping, no separate ack)
   - Capability impact: `max_short = ~2032`, `max_bcopy = seg_size` (default 8 KiB; could be raised)
   - Constraints respected: NC mapping, bus fences, two-phase init for desc pool header, generation tokens unchanged

2. **Rubber-duck the design** (per AGENTS.md mandatory workflow) before coding, especially:
   - Is desc lifetime really 1:1 with FIFO slot, or do we need a separate free path?
   - How does v2 interact with pending_add (now BUSY) when desc pool is full vs FIFO is full?
   - What changes in iface_addr if anything (probably nothing; geometry already exchanged)

3. **Implement v2 am_bcopy**:
   - Extend iface_config: seg_size, num_segs
   - Extend region layout: add desc area after FIFO area
   - Extend pool: desc pool with bitmap + alloc/free
   - Modify ep_am_bcopy: alloc desc → pack_cb(desc, arg) → publish elem with desc-index
   - Modify iface_progress BCOPY branch: read desc-index from elem → invoke_am(desc, length) → free desc (or via owner-bit handshake)
   - Update iface_query: max_bcopy = seg_size

4. **Build verify** via `task` agent + `ucx_info -d -t obmm` capability check.
5. **code-review** the diff.
6. After all of the above passes, ask user to deploy and run existing MPI tests (sizes still ≤ 1900 — should still pass; v2 just unlocks larger sizes for future test extension).
</next_steps>