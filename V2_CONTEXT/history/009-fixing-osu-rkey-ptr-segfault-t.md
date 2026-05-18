<overview>
Task: implement v2 obmm UCT transport in UCX (am_short + am_bcopy), then validate with MPI tests + OSU benchmarks. We're now in the OSU triage phase — fixing transport issues exposed by OSU one bug at a time. Hardware (libobmm cross-host shared memory) is NOT available locally; user runs on ARM64 nodes node1/node2 and reports back. Mandatory workflow per AGENTS.md: vector-DB retrieval, rubber-duck plans, code-review diffs, do not modify outside `ucx/src/uct/obmm/`.
</overview>

<history>
1. v2 am_bcopy design + impl (already complete in prior summary)
   - DESIGN.md, pool version 1→2, 1:1 desc-paired-FIFO-elem layout, full bus fence, etc.
   - User confirmed pool version mismatch (got 1, expected 2) at first run — FIX was to manually zero the pool header. Created `obmm_pool_reset.c` (root dir) — 1 MiB zero of mmap'd chardev. User ran it, all v1 MPI tests still pass with v2.

2. User asked for v2 MPI test suite covering am_bcopy
   - Created 3 new tests at repo root:
     - `mpi_correctness_v2.c` — sizes 1B → 1MiB, hits short/bcopy/frag boundaries (2031/2032/2033, 4095/4096/4097), WINDOW=16 concurrent Isend/Irecv, recv buf preset to 0xCD, 5-dim pattern (iter,size,src,dst,off via simpler 3-dim here)
     - `mpi_pingpong_v2.c` — same size sweep, latency
     - `mpi_bw_v2.c` — same sweep with WINDOW=32, adaptive iters
   - Updated `build_mpi_tests.sh` and `run_mpi_tests.sh` (added `ONLY=v2` selector)
   - Updated `MPI_TESTS.md`

3. User asked: "any test for >2 ranks? max ranks supported?"
   - Confirmed `UCT_OBMM_POOL_SLOT_COUNT = 256` compile-time → 256 ranks/node, 512 total on 2 nodes
   - Created `mpi_multi_v2.c`: N-rank ring + alltoall, sizes to 64KiB, deterministic 5-dim pattern
   - Updated scripts: `MULTI_NP` env (default 8), `run_one` now takes per-test np
   - User reported: all v2 tests pass first try

4. User asked for OSU test recipe
   - Created `run_osu_tests.sh` (root). Originally assumed standard install layout `$OSU_DIR/libexec/osu-micro-benchmarks/mpi/{pt2pt,collective,...}`
   - User clarified: their layout is `$OSU_DIR/{pt2pt,collective,startup}/osu_*` — fixed.
   - Script covers pt2pt (osu_latency, osu_bw, osu_bibw, osu_multi_lat, osu_mbw_mr, osu_latency_mt/mp), 13 blocking collectives, 12 non-blocking collectives, startup. Documents which OSU benchmarks are intentionally skipped (RMA — obmm has no put/get/atomic; GPU; UPC/SHMEM/Java/Python; etc.) and per-node 256 hard cap.

5. User: "osu_latency at size 262144 segfaults — stack: ucp_proto_rndv_progress_rkey_ptr → ucp_datatype_iter_unpack → ... → memcpy_aarch64_sve, addr not mapped"
   - Diagnosed (read code, did NOT speculate per AGENTS.md "下定论前先看代码"): obmm component advertises `UCT_COMPONENT_FLAG_RKEY_PTR` and uses `uct_sm_rkey_ptr` for `.rkey_ptr` op — but obmm only mmaps the FIFO region, NOT the peer's user heap. UCP rndv > ~256K threshold picks rndv-via-rkey_ptr and memcpy's from a peer-VA pointer that isn't mapped locally → segfault.
   - Fixed `ucx/src/uct/obmm/base/obmm_md.c`:
     - `uct_obmm_md_query`: removed `UCT_MD_FLAG_REG | UCT_MD_FLAG_NEED_RKEY`, set `flags=0`, cleared `reg_mem_types/reg_nonblock_mem_types/cache_mem_types`
     - Component `.flags`: `UCT_COMPONENT_FLAG_RKEY_PTR` → 0
     - Component `.rkey_ptr`: `uct_sm_rkey_ptr` → `ucs_empty_function_return_unsupported`
     - Removed unused `#include <uct/sm/base/sm_md.h>`
   - Expected behavior after fix: > 256K falls back to rndv-AM (slower but correct). User confirmed osu_latency and osu_bw now pass.

6. **CURRENT TURN**: user reports `osu_bibw` at size 4096 hangs (no error). UCX_LOG=trace shows last lines:
   ```
   ucp_ep.c:405 created ep ... from api call
   wireup.c:1218 ...
   wireup.c:1241 ...
   wireup.c:1244 ep ...: err mode 0, flags 0x0
   ```
   — so wireup completed and the hang happens in actual data transfer. Not yet diagnosed.
</history>

<work_done>
Files updated this session (post-prior-summary):
- `ucx/src/uct/obmm/base/obmm_md.c`: removed bogus rkey_ptr/REG/NEED_RKEY advertisement (3 spots: md_query attrs, component flags, component.rkey_ptr op + dropped sm_md.h include)
- `obmm_pool_reset.c` (NEW, root): standalone tool to zero pool header after version bump
- `mpi_correctness_v2.c`, `mpi_pingpong_v2.c`, `mpi_bw_v2.c`, `mpi_multi_v2.c` (NEW, root): v2 MPI tests
- `build_mpi_tests.sh`: added all 4 new targets
- `run_mpi_tests.sh`: per-test np via `run_one`, `MULTI_NP` env, `ONLY=v2` selector
- `MPI_TESTS.md`: updated table + ONLY=v2 example
- `run_osu_tests.sh` (NEW, root): OSU runner with category gating, NP_PT2PT/NP_PAIRS/NP_COLL knobs, fixed for `$OSU_DIR/{pt2pt,collective,startup}/` layout

Test status:
- [x] all v1 MPI tests pass cross-node
- [x] all v2 MPI tests pass cross-node (including mpi_multi_v2 with default NP=8)
- [x] osu_latency, osu_bw pass (after rkey_ptr fix)
- [ ] osu_bibw HANGS at size=4096 — UNDIAGNOSED, this is the next item
- [ ] remaining OSU tests untested
</work_done>

<technical_details>
- **rkey_ptr trap**: the prior obmm code copy-pasted from `uct/sm/base/sm_md.c` advertising RKEY_PTR + uct_sm_rkey_ptr — this is correct for sysv/posix/xpmem (whole peer process is attached) but WRONG for obmm where only the 128MiB FIFO region is mmap'd. Symptom: `ucp_proto_rndv_progress_rkey_ptr` segfaults at ~256K (rndv threshold). Fix doesn't break anything — UCP falls back to rndv-AM.
- **rndv threshold**: UCX default is ~256 KiB. Below: eager (am_short ≤2032, am_bcopy + UCP frag above). Above: rendezvous.
- **osu_bibw 4096 hang clue**: bibw (bidirectional bandwidth) does WINDOW Isends in both directions simultaneously. Hang at 4096 (= default BCOPY_SEG_SIZE!) is suspicious. Possible causes to investigate:
  - **Most likely: FIFO deadlock under symmetric back-pressure**. Both ends reserve all FIFO_SIZE=64 slots in their peer's queue with bcopy sends, then both progress loops are blocked on `am_bcopy` returning UCS_ERR_NO_RESOURCE / pending → BUSY-retry. UCP's pending arbiter returns `UCS_ERR_BUSY` (known limitation flagged in DESIGN.md). Symmetric all-or-nothing batching may not allow either side to drain before pending fires.
  - Could also be a barrier issue at the size transition where bibw resets state.
  - Need to check `osu_bibw` source (it does WINDOW=64 Isend both ways then Waitall — at 4096B*64 = 256KiB in flight per direction = exactly at rndv threshold).
- **AGENTS.md hard rule**: don't modify outside `ucx/src/uct/obmm/` without asking the user. Drove the local `uct_obmm_bus_full_fence` helper. The rkey_ptr fix stayed inside obmm_md.c.
- **Pool version bump procedure**: when layout changes, `obmm_pool_reset.c` must be run on both nodes (with no jobs running) before next test. Working now without further bumps.
- **OSU layout user-specific**: `$OSU_DIR/pt2pt/osu_latency` etc. (not the autotools-install layout).

Open question for the bibw hang: need to read OSU's osu_bibw source AND check what UCP does when both endpoints hit `pending` simultaneously. Don't speculate — trace the code.
</technical_details>

<important_files>
- `ucx/src/uct/obmm/base/obmm_md.c`
   - Just fixed: removed rkey_ptr advertisement
   - md_query (~line 28-46): flags=0, only access_mem_types set
   - md_ops (~line 115): unchanged
   - component (~line 214): flags=0, rkey_ptr=ucs_empty_function_return_unsupported
- `ucx/src/uct/obmm/base/obmm_iface.c`
   - cap.flags at ~line 81 (need to verify what it advertises — could be relevant to bibw hang if any flag implies symmetric assumption)
   - iface_progress (~line 200-260): receive loop, may need to check if it drains both directions adequately
   - init validation around lines 280-380
- `ucx/src/uct/obmm/base/obmm_ep.c`
   - `uct_obmm_ep_am_short` (load+CAS slot reservation)
   - `uct_obmm_ep_am_bcopy` (~lines 257-330)
   - `uct_obmm_ep_pending_add` returns `UCS_ERR_BUSY` (no real arbiter — flagged limitation)
- `ucx/src/uct/obmm/DESIGN.md`
   - Single source of truth for v2 design
- `ucx/src/uct/obmm/base/obmm_fifo.h`
   - `uct_obmm_bus_full_fence()` definition
   - slot helpers (stride, descs, desc-by-index)
- root: `mpi_*.c`, `run_mpi_tests.sh`, `run_osu_tests.sh`, `MPI_TESTS.md`, `obmm_pool_reset.c`
- `AGENTS.md` — mandatory workflow, hard rule about obmm dir scope
- `.github/skills/ucx-build-verify/SKILL.md` — build/verify checklist
</important_files>

<next_steps>
Immediate task: diagnose `osu_bibw` hang at size=4096.

Approach (DO NOT speculate per AGENTS.md "下定论前先看代码"):
1. Look up OSU `osu_bibw` source (vector-DB or web) to confirm exact send pattern and WINDOW size.
2. Re-read `uct_obmm_ep_am_bcopy` for the BUSY return path and `uct_obmm_iface_progress` for drain semantics.
3. Check whether obmm advertises any cap (e.g. `UCT_IFACE_FLAG_AM_DUP`, `UCT_IFACE_FLAG_PUT_*`) that would make UCP send a control message expecting an ACK that never comes.
4. Hypothesis to verify (not assume): symmetric WINDOW Isends fill both peers' FIFOs → both am_bcopy returns NO_RESOURCE → UCP queues pending → pending_add returns BUSY → UCP retries forever, but progress doesn't drain because the receiver side is also blocked sending.
5. If hypothesis holds, options:
   - Implement a minimal real `pending_add` (return UCS_OK + arbiter callback) — biggest fix, mirrors mm_ep
   - Verify is_reachable / wireup is OK at size 4096 (could be wireup size-triggered if at exact bcopy boundary something fragments wrong)
   - Check if 4096 is exactly causing UCP to switch protocols at that boundary

Pending OSU tests to triage after bibw: osu_multi_lat, osu_mbw_mr, all collectives, all non-blocking collectives. User will run them serially and report.

Other deferred:
- Promote `uct_obmm_bus_full_fence` to ucs/arch (needs user approval)
- v3: real RMA (put/get) for proper rndv performance > 256K
</next_steps>