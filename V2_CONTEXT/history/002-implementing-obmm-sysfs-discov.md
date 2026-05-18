<overview>
Implementing the `obmm` UCT transport in UCX (`ucx/src/uct/obmm/`) for cross-host shared memory on the OBMM fabric. v1 only implements `am_short`; testing is no-hardware (build-only). The user has locked in a NC-mapping (`O_SYNC`) data-path design, bus-domain fences for cross-host visibility, slot generation tokens for crash safety, and `(exporter_dcna, exporter_deid, memid)` for reachability. After rubber-duck review, the user said "开干" — actively coding now, currently mid-implementation of the `discovery → region-mmap` phase.
</overview>

<history>
1. User asked for the agent/skill system to be designed first (prior to compaction).
   - Created AGENTS.md + 3 skills, all already on disk.

2. User said "开干" (start work) — prior compaction summary captured this.
   - Did vector-DB retrieval, asked clarifying questions, wrote plan.md, seeded 14 todos.

3. User asked me to verify my CAS-init design against `obmm_set_ownership`.
   - Discovered cacheable consistency model is incompatible with mm-style FIFO; switched to NC mapping with user's blessing.

4. User asked for rubber-duck critique of the plan.
   - Rubber-duck found 5 BLOCKERs + 4 IMPORTANTs.
   - Most critical: `ucs_memory_cpu_*_fence` doesn't cover cross-host NC; need `ucs_memory_bus_*_fence`. Also: two-phase init, slot generation tokens, `iface_progress_enable` must wire to `uct_base_iface_progress_enable_cb`, drop `EP_CHECK`, fix `max_short` formula.
   - Updated `plan.md` and `obmm-api-and-env` skill with all fixes.

5. User asked whether cacheable could still be made to work, and whether UCX has precedent for manual cache management.
   - Investigated: UCX has `ucs_arch_clear_cache` (mfence/clflush on x86; `dc cvau`+`ic ivau` on arm64) but uses it ONLY for instruction cache (JIT/code patching), never on data paths.
   - Three hard blockers for cacheable: (a) OBMM consistency is page-table-level enforced, requires syscall-per-message via `obmm_set_ownership`; (b) arm64 EL0 lacks `dc ivac`; (c) no UCX precedent for cross-host cacheable manual coherence.
   - Recommended staying with NC + bus fences. User confirmed.

6. User said "写进去吧，按 bus fence 走" — lock NC + bus fences in skill.
   - Added "Why cacheable + manual cache management was rejected" section to `obmm-api-and-env/SKILL.md`.
   - Added bus fence requirement to "Forbidden assumptions".

7. User said "开干" again — start implementation phase.
   - Marked `discovery` todo in_progress.
   - Read existing skeleton (md/iface/ep) + sysfs doc.
   - Found UCX helpers: `ucs_sys_readdir`, `ucs_read_file_str`, `ucs_read_file_number` (uses `strtol(..., 0)` so accepts hex prefixes but returns `long` not u64).
   - Created `obmm_sysfs.{h,c}` — full sysfs scanner returning `uct_obmm_dev_info_t[]`.
   - Created `obmm_region.{h,c}` — open(O_SYNC) + mmap wrapper.
   - Wired both into `ucx/src/uct/Makefile.am`.
   - Extended `uct_obmm_md_t` to hold mapped regions; rewrote `uct_obmm_md_open` to discover + map all devices, track which is the local export.
</history>

<work_done>
Files created:
- `ucx/src/uct/obmm/base/obmm_sysfs.h` — public API for sysfs discovery (`uct_obmm_dev_info_t`, `uct_obmm_eid_t`, `uct_obmm_sysfs_discover/release`).
- `ucx/src/uct/obmm/base/obmm_sysfs.c` — full impl using `ucs_sys_readdir` + helpers `uct_obmm_read_u64_hex` (sscanf %SCNx64), `uct_obmm_read_eid` (parses "u64 : u64"), `uct_obmm_parse_memid` (filters `obmm_shmdev<digits>`). Two-pass: load each device, then patch self-dcna into export entries from any import device's `import_info/scna`.
- `ucx/src/uct/obmm/base/obmm_region.h` — `uct_obmm_region_t` + open/close API.
- `ucx/src/uct/obmm/base/obmm_region.c` — `open(dev_path, O_RDWR|O_SYNC|O_CLOEXEC)` + `mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)`.

Files modified:
- `ucx/src/uct/Makefile.am` — added `obmm_sysfs.{h,c}` and `obmm_region.{h,c}`.
- `ucx/src/uct/obmm/base/obmm_md.h` — added `uct_obmm_region_t *regions; unsigned num_regions; int export_idx;` to `uct_obmm_md_t`.
- `ucx/src/uct/obmm/base/obmm_md.c` — rewrote `uct_obmm_md_open` to discover + map; added `uct_obmm_md_unmap_all`, `uct_obmm_md_map_devices`. Closes regions on md_close. If multiple exports exist, warns and uses first. Returns `UCS_ERR_NO_DEVICE` if no devices.

Updated skills/plan:
- `.github/skills/obmm-api-and-env/SKILL.md` — locked-in section now includes (3) two-phase init, (5) self-loopback, (6) generation tokens, (7) bus fences. Added "Why cacheable + manual cache management was rejected" section.
- `C:\Users\zl\.copilot\session-state\b9570082-d3ac-44bc-80c0-7fcb84099c09\plan.md` — updated with all 9 rubber-duck fixes (state machine init, generation+starttime, dcna/deid reachability, max_short formula, drop EP_CHECK, progress wiring, bus fences, cleanup).

SQL todos:
- done: `plan-review`, `discovery`
- ready: `region-mmap` (just completed but not yet marked), `config-fields`
- pending (with deps): `pool-layout`, `iface-addr`, `fifo-types`, `ep-create`, `am-short-send`, `iface-progress`, `iface-query-caps`, `ops-table-wire`, `build-verify`, `code-review`

Status: NOT YET BUILD-VERIFIED. Code is written but no compile attempted (Windows host has no UCX build env; build will happen at `build-verify` todo on a Linux box).
</work_done>

<technical_details>
**Bus fences vs CPU fences (rubber-duck finding, locked in)**
- `ucs_memory_cpu_store_fence()` = compiler fence on x86, `dmb ish` on arm64 (inner-shareable only — does NOT reach cross-host NC).
- `ucs_memory_bus_store_fence()` = `sfence` on x86, `dmb oshst` on arm64. THIS is what obmm must use.
- `ucs_memory_bus_load_fence()` = `lfence` on x86, `dmb oshld` on arm64.
- mm transport gets away with CPU fences only because mm peers share inner-shareable cache domain. obmm peers don't.

**Why cacheable was rejected (locked in skill)**
1. OBMM cacheable consistency is page-table enforced — even after `clflush`, the other host's PROT_NONE means SIGSEGV. Requires syscall (`obmm_set_ownership`) per message.
2. arm64 EL0 lacks `dc ivac` (only `dc civac`).
3. UCX has zero precedent for cross-host cacheable manual coherence on data path.
4. Bulk-flip ownership is plausible for future put/get but defeats AM short.

**OBMM sysfs format (from `obmm/doc/obmm_shmdev_sysfs.md`)**
- `/sys/devices/obmm/obmm_shmdev<memid>/`
- Root: `type` ("export"|"import"), `size` (hex), `priv_len`, `priv`, `allow_mmap` (0/1)
- `export_info/`: `deid` (u64:u64), `tokenid`, `uba`, `node_mem_size`, `memory_from_user`
- `import_info/`: `pa`, `scna` (u64), `dcna` (u64), `deid` (u64:u64), `seid` (u64:u64), `numa_id`, `preimport`
- For export: `exporter_dcna` is OUR scna (not in export_info — must be derived from any local import_info/scna). `exporter_deid` is in export_info/deid.
- For import: `exporter_dcna`/`exporter_deid` come directly from import_info.
- `dev_path` = `/dev/obmm_shmdev<memid>` opened with `O_RDWR|O_SYNC|O_CLOEXEC` for NC mapping.

**UCX helper utilities used**
- `ucs_sys_readdir(path, cb, ctx)` in `ucs/sys/sys.h:617`. Callback signature: `ucs_status_t (*)(const struct dirent *entry, void *arg)`. Iterates ALL entries including `.`/`..`.
- `ucs_read_file_str(buf, max, silent, fmt, ...)` — null-terminates, returns `ssize_t`.
- `ucs_read_file_number(long*, silent, fmt, ...)` — uses `strtol(..., 0)` so accepts hex; but returns `long`, not u64, so dangerous for full u64 hex values. We use `ucs_read_file_str` + sscanf `%SCNx64` for u64s.
- `ucs_strtrim`, `ucs_snprintf_safe`, `ucs_realloc`, `ucs_calloc` (memtrack-aware).

**Locked-in design decisions in plan.md**
- Defaults: `OBMM_FIFO_ELEM_SIZE=2 KiB`, `OBMM_FIFO_SIZE=64`, slot ≈ 128 KiB.
- Pool init: `state` field (UNINIT=0/INITING=1/READY=2), CAS to INITING, fill geometry, bus fence, store READY. Losers spin on READY.
- Slot meta per slot: `(owner_pid, owner_starttime, generation, state)`. Crash recovery scans bitmap, validates `/proc/<pid>/stat starttime`.
- `iface_addr` widened to: `(exporter_dcna, exporter_deid_hi, exporter_deid_lo, memid, slot_index, generation, pid)`.
- `cap.am.max_short = fifo_elem_size - sizeof(uct_obmm_fifo_element_t)` (UCT contract: includes 8B header).
- Drop `UCT_IFACE_FLAG_EP_CHECK` (the skeleton currently has it set — must remove in iface_query rewrite).
- `iface_progress_enable/disable` must call `uct_base_iface_progress_enable_cb` (not `ucs_empty_function`).

**Skeleton state issues to fix later (current iface still stubs everything)**
- `obmm_iface.c:43-45` advertises `EP_CHECK` — must drop.
- `obmm_iface.c:155-157` `progress_enable/disable` are `ucs_empty_function` — must wire properly.
- `obmm_iface.c:124-126` cleanup is empty — must release iface state once init does anything.
- `obmm_iface.c:109` still calls `uct_sm_iface_is_reachable` — must drop (same-host only).

**Open assumption (not blocking discovery)**
- `import_info/scna` is identical across all import devices on a host (it's the host's own clan network address). Code assumes this; needs validation on hardware.
- If a host has zero imports, self-dcna can't be derived; export entries get `exporter_dcna=0`. For our 2-node env both hosts always import each other, so OK.

**Build state**
- Cannot compile on Windows host. Code must wait for `build-verify` todo on Linux build host.
- Possible build issues to watch: `inttypes.h` missing somewhere (added to obmm_region.c). `dirent` type from `<dirent.h>` (included in obmm_sysfs.c). `va_start/va_end` requires `<stdarg.h>` — transitively included via stdio but should add explicitly if any error. `vsnprintf` from `<stdio.h>` (included).
</technical_details>

<important_files>
- `C:\Users\zl\Desktop\ucx_obmm_br\AGENTS.md`
   - Mandatory workflow rules (retrieve → re-read env → plan → rubber-duck → implement → verify → review).
   - No changes this session.

- `C:\Users\zl\Desktop\ucx_obmm_br\.github\skills\obmm-api-and-env\SKILL.md`
   - Single source of truth for env + locked decisions.
   - Updated: locked-in items 3,5,6,7 (two-phase init, generation tokens, bus fences). Added "Why cacheable was rejected" section. Item Q3 in "What to ASK" marked RESOLVED.

- `C:\Users\zl\.copilot\session-state\b9570082-d3ac-44bc-80c0-7fcb84099c09\plan.md`
   - Implementation plan with all rubber-duck fixes folded in.
   - Sections 1–9 updated; "Resolved decisions" updated with bus fences + dcna/deid reachability.

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_sysfs.{h,c}` (NEW)
   - Sysfs discovery module. Public types: `uct_obmm_eid_t`, `uct_obmm_dev_info_t`, enum `uct_obmm_dev_type_t`. Public funcs: `uct_obmm_sysfs_discover()`, `uct_obmm_sysfs_release()`.
   - Two-pass: load_one per device, then patch_self_dcna for exports.

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_region.{h,c}` (NEW)
   - `uct_obmm_region_t` holds {info, fd, base, length}. `uct_obmm_region_open` does open(O_SYNC)+mmap; `uct_obmm_region_close` does munmap+close.

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_md.{h,c}`
   - md.h: `uct_obmm_md_t` extended with `regions`, `num_regions`, `export_idx`.
   - md.c: rewrote `md_open` (~lines 100-160) to discover+map; added `uct_obmm_md_unmap_all`, `uct_obmm_md_map_devices`. `md_close` now `ucs_derived_of`'s and unmaps before free.

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\Makefile.am`
   - Added 4 lines (obmm_sysfs.h, obmm_region.h, obmm_sysfs.c, obmm_region.c) in the appropriate header/source lists.

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_iface.{h,c}` and `obmm_ep.{h,c}`
   - Still skeleton. Will be heavily rewritten in upcoming todos. Known issues listed in Technical Details.

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\sm\mm\base\mm_ep.c` and `mm_iface.{c,h}`
   - Reference impl to mirror. Especially `mm_ep.c:300-411` (am_short send path) and `mm_iface.c:198-200,348-357,526-528,889-903`.
</important_files>

<next_steps>
Immediate next step (currently mid-flight when compaction triggered):
- Mark `region-mmap` todo as done in SQL (the implementation IS complete — `obmm_region.{h,c}` + md_open integration, just hadn't run the UPDATE yet).
- Then query for ready todos: should be `pool-layout` + `config-fields` (both depend only on done items now).

Then proceed in dependency order:
1. **`config-fields`** — add `OBMM_FIFO_ELEM_SIZE` (default 2 KiB), `OBMM_FIFO_SIZE` (default 64), `OBMM_FIFO_MAX_POLL`, `OBMM_SEG_SIZE` to `uct_obmm_iface_config_t` (mirror `mm_iface_config_t`).
2. **`pool-layout`** — design + define `uct_obmm_pool_hdr_t` (with `state`, `version`, `slot_size`, `slot_count`, `alloc_bitmap[]`, `slot_meta[]`) and helpers for two-phase init, slot allocation with PID+starttime scavenging, slot free with DEAD/generation bump. Goes in new `obmm_pool.{h,c}`.
3. **`fifo-types`** — `uct_obmm_fifo_ctl_t` (head/tail), `uct_obmm_fifo_element_t` (am_id, length, generation, flags, payload). Mirror `mm_iface.h` types.
4. **`iface-addr`** — widen `uct_obmm_iface_addr_t` from u64 to struct with (exporter_dcna, exporter_deid, memid, slot_index, generation, pid).
5. **`ep-create`** — extend `uct_obmm_ep_t` with `peer_fifo_ctl`, `peer_fifo_elems`, `cached_tail`, `expected_generation`. Look up peer region in MD's mapping table by (dcna, deid, memid).
6. **`iface-progress`** — receive-side polling with bus_load_fence + generation check. Wire `progress_enable/disable` to `uct_base_iface_progress_enable_cb`.
7. **`iface-query-caps`** — set `UCT_IFACE_FLAG_AM_SHORT`, correct `max_short` formula, drop `EP_CHECK`. Replace `uct_sm_iface_is_reachable` with `(exporter_dcna,exporter_deid,memid)` lookup against MD's regions.
8. **`am-short-send`** — implement `uct_obmm_ep_am_short` mirroring `uct_mm_ep_am_short` but with bus fences and generation stamping.
9. **`ops-table-wire`** — replace stubs with real funcs in `uct_obmm_iface_ops`.
10. **`build-verify`** — user runs on Linux: `./autogen.sh && ./contrib/configure-devel && make -j && ucx_info -d -t obmm`.
11. **`code-review`** — invoke code-review agent on full diff.

Open questions (non-blocking):
- Element/payload alignment requirement on obmm hardware (default plan: 64B align).
- Whether slot_size default of 128 KiB is actually optimal; user accepted but it's somewhat arbitrary.
</next_steps>