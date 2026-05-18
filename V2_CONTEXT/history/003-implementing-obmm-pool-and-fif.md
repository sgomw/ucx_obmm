<overview>
Implementing the `obmm` UCT transport in UCX (`ucx/src/uct/obmm/`) for cross-host shared memory on the OBMM fabric. v1 only implements `am_short`; testing is build-only (no hardware). Design locked in: NC mapping (`O_SYNC`) data path, bus-domain fences for cross-host visibility, slot generation tokens for crash safety, two-phase pool init with CAS, `(exporter_dcna, exporter_deid, memid)` for reachability. Currently mid-implementation: foundation (discovery + region mmap + pool + FIFO types + config) is built and compiles; the connecting glue (iface init, ep, send, progress, query, ops table) is the next big push.
</overview>

<history>
1. User said "开干" — continue implementation. Verified region-mmap was complete, marked done.

2. User flagged "import_info has dcna/deid/pa/scna/seid; export_info has only deid/memory_from_user/node_mem_size/tokenid/uba; allow_mmap is at root not inside *_info"
   - Verified my obmm_sysfs.c already matches this layout exactly. No code changes needed.

3. User confirmed "编译通过，继续" — proceed with config-fields/pool-layout/fifo-types phase.
   - Read mm_iface.h reference to understand FIFO design pattern.
   - Created `obmm_fifo.h` (types only): `uct_obmm_fifo_ctl_t` with cacheline-padded head/tail, `uct_obmm_fifo_element_t` with flags/am_id/length/generation/header.
   - Created `obmm_pool.{h,c}`: pool header with two-phase CAS init (state UNINIT→INITING→READY), bitmap-based slot allocator with PID+starttime liveness checking via `ucs_sys_get_proc_create_time`, generation bump on free/scavenge.
   - Added `OBMM_FIFO_SIZE=64`, `OBMM_FIFO_ELEM_SIZE=2048`, `OBMM_FIFO_MAX_POLL=16` to iface config table.
   - Wired new files into Makefile.am.

4. User reported compile error: "missing declaration of ucs_align_up"
   - Added `#include <ucs/sys/ptr_arith.h>` to obmm_pool.c. (`ucs_align_up` lives in ptr_arith.h, not math.h.)

5. User reported "taking address of packed member" warnings on `&hdr->state` (line 88, 135) and "unused variable meta_off" (line 84)
   - Removed `UCS_S_PACKED` from `uct_obmm_pool_hdr_t` and `uct_obmm_slot_meta_t` (their natural alignment is fine; packed makes `&hdr->state` look misaligned to compiler, breaks atomic CAS on NC mapping arm64).
   - Removed unused `meta_off` local variable.

6. User asked "你确定meta_off不是你忘写逻辑了"
   - Traced through: memset zeros bitmap+meta in one shot starting at `hdr+sizeof(hdr)`; slot_off is computed via helper that already includes meta size; attach() recomputes meta location from slot_count. Confirmed meta_off was genuinely dead.

7. User said "ok，编译通过，继续吧" — start the next big block.
   - Read existing skeleton obmm_ep.h/c and obmm_md.c to plan the changes. (THIS is where context was compacted.)
</history>

<work_done>
Files created this session:
- `ucx/src/uct/obmm/base/obmm_fifo.h` — FIFO ctl + element types, slot helpers
- `ucx/src/uct/obmm/base/obmm_pool.h` — pool hdr + slot meta types, attach/alloc/free API
- `ucx/src/uct/obmm/base/obmm_pool.c` — two-phase init + bitmap allocator + scavenger

Files modified this session:
- `ucx/src/uct/obmm/base/obmm_iface.h` — added `fifo_size`, `fifo_elem_size`, `fifo_max_poll` to config struct
- `ucx/src/uct/obmm/base/obmm_iface.c` — added 3 entries to config table
- `ucx/src/uct/Makefile.am` — wired in `obmm_fifo.h`, `obmm_pool.h`, `obmm_pool.c`

Build status: COMPILES on user's Linux build host (user confirmed twice).

Todo status (6 done, 8 pending):
- ✅ done: plan-review, discovery, region-mmap, config-fields, pool-layout, fifo-types
- ⬜ pending: iface-addr, ep-create, am-short-send, iface-progress, iface-query-caps, ops-table-wire, build-verify, code-review

Most recent action: read obmm_ep.h, obmm_ep.c, obmm_md.c to plan the next block. Was about to start writing iface-addr widening + ep restructuring + am_short_send + progress + query rewrite + ops table wire — all 6 remaining implementation todos are tightly coupled and planned to land in a single push.
</work_done>

<technical_details>
**Pool design (locked in, implemented):**
- `uct_obmm_pool_hdr_t`: magic / version / state / slot_count / slot_size / slot_array_offset / bitmap_words / initializer_pid / initializer_starttime + reserved
- `uct_obmm_slot_meta_t`: state(FREE/IN_USE/DEAD) / generation / owner_pid / reserved / owner_starttime
- Layout: `[hdr][bitmap u64s][slot_meta[]]<cacheline pad>[slot 0..N-1]`
- Init: CAS state UNINIT→INITING; winner zeros bitmap+meta in one memset, fills geometry, bus_store_fence, sets READY; loser spins, periodically checks initializer pid+starttime, CAS back to UNINIT if dead; ~4M iter timeout.
- Alloc: scan bitmap; for set bits, check owner liveness via `ucs_sys_get_proc_create_time` and starttime match; for free bits, CAS to set; ALWAYS bump generation, then memset slot bytes, then bus fence, then mark IN_USE — order matters because racing stale writers' bytes won't survive un-stamped (but memset happens AFTER gen bump so stale writes after memset land with old gen and get dropped at receive).
- Free: bump generation, mark DEAD, bus fence, CAS-clear bitmap bit, mark FREE.

**Critical: NO `UCS_S_PACKED` on pool_hdr or slot_meta.** Fields are naturally aligned by design; packed attribute makes compiler treat `&hdr->state` as potentially misaligned, which breaks atomic CAS on NC mappings on arm64 (Device memory unaligned access faults). FIFO_element keeps `UCS_S_PACKED` because it's wire format and not subject to atomic operations on individual fields.

**Header locations:**
- `ucs_align_up` is in `ucs/sys/ptr_arith.h`, NOT `ucs/sys/math.h`
- `ucs_sys_get_proc_create_time(pid_t pid)` in `ucs/sys/sys.h:674` returns 0 if pid dead/unreadable
- `ucs_atomic_cswap32/64` in `ucs/arch/atomic.h`
- `ucs_memory_bus_store_fence/ucs_memory_bus_load_fence` in `ucs/arch/cpu.h` (sfence/lfence on x86, dmb oshst/oshld on arm64)
- No `ucs_cpu_relax` exists — spin loops just don't have a relax hint (removed).

**Sysfs layout (verified with user):**
- root: type, size, allow_mmap, priv_len, priv (allow_mmap is at ROOT not in *_info)
- export_info/: deid, memory_from_user, node_mem_size, tokenid, uba (NO scna/dcna here)
- import_info/: pa, scna, dcna, deid, seid, [numa_id], [preimport]
- self_dcna for export devices is derived from any import device's import_info/scna (two-pass scan in obmm_sysfs.c).

**Plan for the upcoming block:**

1. **iface-addr**: widen `uct_obmm_iface_addr_t` from `uint64_t` to struct:
   ```c
   typedef struct {
       uint64_t exporter_dcna;
       uint64_t exporter_deid_hi;
       uint64_t exporter_deid_lo;
       uint32_t slot_index;
       uint32_t generation;
       uint32_t pid;
       uint32_t fifo_size;
       uint32_t fifo_elem_size;
       uint32_t pad;
   } uct_obmm_iface_addr_t;
   ```
   (Include fifo_size/elem_size so peer can validate geometry before trusting slot layout.)

2. **iface init/cleanup**: in `UCS_CLASS_INIT_FUNC(uct_obmm_iface_t)`, get MD, find local export region (`md->regions[md->export_idx]`), `uct_obmm_pool_attach`, `uct_obmm_pool_alloc_slot`, store slot_index/generation/pool/fifo_ctl/fifo_elems pointers, init read_index, fifo_mask=fifo_size-1. Cleanup: free slot.

3. **ep-create**: extend `uct_obmm_ep_t` with peer_fifo_ctl, peer_fifo_elems, cached_tail, expected_generation, fifo_size, fifo_mask, fifo_elem_size. In init, parse remote iface_addr, scan MD's regions for matching (exporter_dcna, exporter_deid_hi, exporter_deid_lo) — that's our import for that peer. Compute slot pointer via pool offset math (slot_index * slot_size + slot_array_offset). Cache geometry from iface_addr (validated against local config or accepted).

4. **am-short-send**: mirror `uct_mm_ep_am_short` but with bus fences. FAA on `ep->peer_fifo_ctl->head`; check head-cached_tail < fifo_size (else update cached_tail then NO_RESOURCE); compute elem ptr `peer_elems + (head & fifo_mask) * elem_size`; write am_short header + payload + length + am_id + generation; bus_store_fence; set OWNER bit (toggles per wrap = `(head & fifo_size) ? OWNER : 0`); return UCS_OK.

5. **iface-progress**: poll local recv FIFO. Read head with bus_load_fence. For each elem from read_index up to head (bounded by fifo_max_poll), check OWNER flag matches expected polarity, check generation matches own slot's meta.generation (drop if stale), `uct_iface_invoke_am`, advance read_index. Periodically write tail.

6. **iface-query-caps**: set `UCT_IFACE_FLAG_AM_SHORT | UCT_IFACE_FLAG_CONNECT_TO_IFACE | UCT_IFACE_FLAG_CB_SYNC`, drop `EP_CHECK`. `cap.am.max_short = fifo_elem_size - sizeof(uct_obmm_fifo_element_t)` (UCT contract: max_short includes 8B header). iface_addr_len = sizeof(struct).

7. **ops-table-wire**: replace stubs for `ep_am_short`, `iface_progress`, `iface_progress_enable=uct_base_iface_progress_enable_cb`, `iface_progress_disable=uct_base_iface_progress_disable_cb`. Drop `uct_sm_iface_is_reachable` from is_reachable_v2 (replace with (dcna,deid)-in-MD lookup).

**Open assumptions not yet resolved:**
- assuming all imports on a host share same scna (filling self_dcna for exports)
- assuming receiver and sender configure same fifo_size/elem_size (will validate via iface_addr fields)
- single MD per host, and we use `regions[export_idx]` as THE local export pool
</technical_details>

<important_files>
- `C:\Users\zl\.copilot\session-state\b9570082-d3ac-44bc-80c0-7fcb84099c09\plan.md`
   - Implementation plan with all 9 rubber-duck fixes folded in
   - Read before resuming for full design context

- `C:\Users\zl\Desktop\ucx_obmm_br\.github\skills\obmm-api-and-env\SKILL.md`
   - Single source of truth for env + locked decisions (NC + bus fences, generation tokens, two-phase init, dropped EP_CHECK)

- `C:\Users\zl\Desktop\ucx_obmm_br\AGENTS.md`
   - Mandatory workflow rules

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_pool.{h,c}` (NEW, COMPILES)
   - Cross-process slot allocator; central to data path correctness
   - `obmm_pool.h`: pool_hdr_t (NO PACKED), slot_meta_t (NO PACKED), pool_t accessor, required_size/attach/alloc_slot/free_slot/slot_ptr API
   - `obmm_pool.c`: init_or_wait with CAS+initializer-liveness recovery, try_claim with scavenge, all bus_store_fence not cpu_store_fence

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_fifo.h` (NEW, COMPILES)
   - Wire types: fifo_ctl_t (cacheline-padded head/tail), fifo_element_t (UCS_S_PACKED with flags/am_id/length/generation/header)
   - Inline helpers: slot_stride/ctl/elems/elem

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_iface.{h,c}`
   - Config fields added but iface init/query/ops still skeleton
   - obmm_iface.h:16 `uct_obmm_iface_addr_t` is still `uint64_t` — must widen
   - obmm_iface.h:18-26 config has fifo_size/elem_size/max_poll added
   - obmm_iface.c:22-43 config table has 3 new entries
   - obmm_iface.c:43-45 still advertises EP_CHECK — must drop in query rewrite
   - obmm_iface.c:51-56 cap.am.max_short=0 — must compute as elem_size-sizeof(elem_t)
   - obmm_iface.c:109 `uct_sm_iface_is_reachable` — must drop (same-host only)
   - obmm_iface.c:115-126 init/cleanup are stubs — must wire pool attach + slot alloc
   - obmm_iface.c:155-157 progress_enable/disable are ucs_empty_function — must wire to base callbacks
   - obmm_iface.c:151-157 ops table stubs — must replace ep_am_short, iface_progress

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_ep.{h,c}`
   - Skeleton; just super struct, no peer pointers
   - obmm_ep.h:13-15 must extend with peer_fifo_ctl, peer_fifo_elems, cached_tail, expected_generation, fifo_size/mask/elem_size
   - obmm_ep.c:15-22 init must parse remote iface_addr and find matching MD region
   - obmm_ep.c:32-46 is_connected uses old `*addr == iface->id` comparison — must rewrite for new struct addr

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_md.{h,c}`
   - md.c: `uct_obmm_md_open` discovers + maps all regions, tracks `export_idx`
   - md.h:19-24 `uct_obmm_md_t` has `regions`, `num_regions`, `export_idx`
   - Used as the source of truth for which region serves which (dcna,deid)

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_sysfs.{h,c}`
   - Discovery (DONE, COMPILES). `uct_obmm_dev_info_t` has type, memid, size, allow_mmap, dev_path, exporter_dcna, exporter_deid (with hi/lo).

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_region.{h,c}`
   - mmap wrapper (DONE, COMPILES). `uct_obmm_region_t` carries info copy + fd + base + length.

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\sm\mm\base\mm_ep.c` and `mm_iface.{c,h}`
   - REFERENCE to mirror. Especially `mm_ep.c:280-411` (am_short), `mm_iface.c:35-66` (config table), `mm_iface.c:648-816` (init + set_fifo_ptrs).
</important_files>

<next_steps>
Currently mid-flight: read obmm_ep.h, obmm_ep.c, obmm_md.c — about to start the 6-todo block in one push.

Immediate next steps in order:
1. **iface-addr**: widen `uct_obmm_iface_addr_t` in obmm_iface.h from u64 to struct with (exporter_dcna, exporter_deid_hi, exporter_deid_lo, slot_index, generation, pid, fifo_size, fifo_elem_size, pad).

2. **iface init rewrite** (obmm_iface.c UCS_CLASS_INIT_FUNC + CLEANUP_FUNC):
   - Get MD, use `md->regions[md->export_idx]` if `export_idx >= 0` else fail UCS_ERR_NO_DEVICE
   - Allocate `uct_obmm_pool_t` storage in iface
   - `uct_obmm_pool_attach(region.base, region.length, fifo_size_for_pool, slot_size, &iface->pool)` — but slot_count here is pool-wide (how many ifaces can share this region), separate from per-slot fifo_size. Need a separate config: `OBMM_POOL_SLOT_COUNT` (default 64?). Hmm — review plan, may have already decided this.
   - `uct_obmm_pool_alloc_slot` → get slot_index, slot_ptr, generation
   - Set up local `recv_fifo_ctl = uct_obmm_slot_ctl(slot_ptr)`, `recv_fifo_elems = uct_obmm_slot_elems(slot_ptr)`, `read_index=0`, `fifo_mask=fifo_size-1`
   - Populate `iface->self_addr` for get_address
   - On cleanup: `uct_obmm_pool_free_slot`

3. **ep-create**: extend obmm_ep_t, parse remote iface_addr in init, scan `md->regions` for matching (dcna, deid), compute peer slot pointers via `uct_obmm_pool_attach` (or just slot_array_offset math against the peer region — peer's pool will be initialized by peer; we attach as a reader).

4. **am-short-send**: implement `uct_obmm_ep_am_short` with FAA + bus_store_fence + OWNER bit toggle.

5. **iface-progress**: receive loop with bus_load_fence + generation check + AM dispatch + tail update.

6. **iface-query-caps + ops-table-wire**: drop EP_CHECK, set AM_SHORT, fix max_short, drop sm_iface_is_reachable, wire real funcs.

Open question to resolve while implementing: **pool slot_count config**. The pool inside the local export region must hold one slot per local iface. Need a separate `OBMM_POOL_SLOTS` config (default e.g. 64) — separate from `OBMM_FIFO_SIZE` (which is depth of one ring). Likely add to md_config (since pool geometry is region-wide and shared across all ifaces) or to iface_config (with caveat that first iface to attach wins). The plan doc may already state this; check before adding.

Then send for code-review and finally build-verify on Linux.
</next_steps>