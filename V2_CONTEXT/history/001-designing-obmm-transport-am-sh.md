<overview>
The user is developing a new UCT transport layer called `obmm` inside UCX, on top of the libobmm shared-memory fabric, to support HPC/MPI workloads on a 2-node test environment (each node has pre-exported a 128 MiB region and pre-imported the peer's). Only `am_short` is to be implemented for v1; no hardware is available for runtime testing. The first phase was to **build a durable agent/skill system** to guide development; the second phase (currently in progress) is the actual am_short implementation, currently paused at the rubber-duck checkpoint after design decisions were locked in with the user.
</overview>

<history>
1. The user described the project and asked to design an agent/skill system before any coding
   - Explored the three repos (`ucx/`, `obmm/`, `ompi/`) and existing `vector-db-retrieval` skill
   - Confirmed scope with user via ask_user (chose "3 skills + AGENTS.md")
   - Created 3 new skills + AGENTS.md hard-rules file

2. The user said "开干" (start work)
   - Per AGENTS.md workflow: did vector-DB retrieval on `uct_mm_ep_am_short` and `uct_mm_iface_progress`
   - Asked user a chain of clarifying questions about environment (device path, memid distribution, topology)
   - Discovered key facts: device is `/dev/obmm_shmdev{memid}`, memids found via sysfs scan, each node exports 1 region, every process imports both (self + peer)
   - Wrote `plan.md` in session folder + seeded 14 todos in SQL with dependencies
   - Listed 6 open questions for user review

3. The user told me to read `obmm_set_ownership.md` first to check for conflicts with my CAS-based concurrency design
   - Read `obmm_set_ownership.md` and `libobmm.md`
   - Discovered MAJOR conflict: cacheable OBMM mappings have a host-level mutually-exclusive consistency model — writer-on-host-A + reader-on-host-B simultaneously is forbidden, which breaks the mm receive-FIFO model
   - Proposed switching the data path to NC (`O_SYNC`) mapping
   - User accepted NC, then asked whether software can guarantee atomicity → I analyzed: SPSC with fences works, RMW (FAA/CAS) needs hardware
   - User chose option B: stay with mm-style multi-producer FIFO and explicitly **guarantees** that obmm NC supports cross-node atomic FAA
   - Confirmed remaining decisions: UCT owns 128 MiB layout, region is zero-filled, default `elem_size=2 KiB` `depth=64` (after I admitted my initial 64 KiB choice was arbitrary and re-derived defaults from obmm vs mm differences)
   - Updated `plan.md` and the `obmm-api-and-env` skill to lock in all decisions
</history>

<work_done>
Files created:
- `C:\Users\zl\Desktop\ucx_obmm_br\AGENTS.md` — mandatory workflow rules (retrieve → re-read env → plan → rubber-duck → implement → build-verify → code-review)
- `C:\Users\zl\Desktop\ucx_obmm_br\.github\skills\uct-transport-patterns\SKILL.md` — UCT framework contracts + reference transports
- `C:\Users\zl\Desktop\ucx_obmm_br\.github\skills\obmm-api-and-env\SKILL.md` — libobmm API + environment facts (since updated with locked-in decisions)
- `C:\Users\zl\Desktop\ucx_obmm_br\.github\skills\ucx-build-verify\SKILL.md` — no-hardware verification checklist
- `C:\Users\zl\.copilot\session-state\b9570082-d3ac-44bc-80c0-7fcb84099c09\plan.md` — am_short v1 plan (since updated with resolved decisions)

SQL todos seeded (14 total, with dependency graph):
- `plan-review` (in_progress), `discovery`, `region-mmap`, `pool-layout`, `iface-addr`, `fifo-types`, `ep-create`, `am-short-send`, `iface-progress`, `iface-query-caps`, `ops-table-wire`, `config-fields`, `build-verify`, `code-review`

Work completed:
- [x] Skill/agent system built and committed
- [x] All vector-DB retrieval done for mm reference
- [x] All open design questions resolved with user
- [x] plan.md and obmm-api-and-env skill updated with locked-in decisions
- [ ] Rubber-duck review of plan (NEXT)
- [ ] Any actual code in `ucx/src/uct/obmm/` (still untouched skeleton)
</work_done>

<technical_details>
**OBMM consistency model — critical finding**
- Cacheable OBMM mappings have host-level mutex: either all hosts read-only/none, or exactly one host has writers and all others must be PROT_NONE
- This makes mm-style "remote writes my receive FIFO, I read it" impossible without expensive ownership flips per message
- **Solution: use NC mapping via `open(... O_SYNC)`** — NC is exempt from the consistency model, all hosts can read/write freely

**User-guaranteed hardware behavior**
- Cross-node atomic FAA/CAS on NC mappings WORKS on this obmm fabric (user explicitly guaranteed when choosing option B)
- This lets us mirror mm transport's multi-producer FIFO design (FAA on shared head) without redesigning to per-ep SPSC

**Locked-in design decisions (in plan.md)**
1. NC mapping (`O_SYNC`) for data path; never call `obmm_set_ownership`
2. Cross-node NC atomic RMW guaranteed
3. UCT owns layout in 128 MiB region; platform export is zero-filled; CAS-on-magic init pattern
4. Per node: 1 export region + 1 import region (peer's export); discovered via `/sys/devices/obmm/obmm_shmdev*/{export_info,import_info}`
5. Self-loopback supported (single-node multi-process case)
6. Defaults: `OBMM_FIFO_ELEM_SIZE=2 KiB`, `OBMM_FIFO_SIZE=64` → ~128 KiB per slot, ~1024 slots in 128 MiB
7. Memory ordering: plain `ucs_memory_cpu_store_fence()` / `_load_fence()` is sufficient (NC bus-level strongly ordered)
8. Reachability: accept any peer whose `iface_addr.local_memid` matches this node's local export OR known peer memid; drop `uct_sm_iface_is_reachable`

**Existing obmm skeleton (in `ucx/src/uct/obmm/base/`)**
- All 3 files exist (`obmm_md.{c,h}`, `obmm_iface.{c,h}`, `obmm_ep.{c,h}`)
- All ep ops are `unsupported`; all caps are 0; `iface_addr_t` is just `uint64_t`
- Built unconditionally via `ucx/src/uct/Makefile.am`; no separate `configure.m4` or `Makefile.am` under obmm/
- libobmm is NOT linked into UCX yet, and v1 won't link it (sysfs + open + mmap is enough)

**Reference patterns confirmed via vector-DB retrieval**
- `uct_mm_ep_am_short` (mm_ep.c:397) — calls `uct_mm_ep_am_common_send(UCT_MM_SEND_AM_SHORT, ...)`
- Sender flow: FAA on `peer_fifo_ctl->head`, write `[am_id|header|payload]` inline into elem, `ucs_memory_cpu_store_fence()`, set owner-bit (flips on each FIFO wraparound)
- mm uses pending queue + arbiter on `NO_RESOURCE` — v1 obmm SKIPS this (just returns `UCS_ERR_NO_RESOURCE`)

**Open questions REMAINING (in skill, not blocking)**
- Element/payload alignment requirements on obmm hardware (default plan: 64 B align)
- Whether NC writes need fabric-level barrier beyond CPU dmb/mfence (user indicated CPU fences are sufficient; reconfirm if symptoms suggest otherwise)
</technical_details>

<important_files>
- `C:\Users\zl\Desktop\ucx_obmm_br\AGENTS.md`
   - Mandatory dev workflow; defines retrieve → plan → rubber-duck → implement → verify → review discipline
   - Hard rules forbidding obmm runtime APIs in transport, mpirun, modifications outside obmm scope

- `C:\Users\zl\Desktop\ucx_obmm_br\.github\skills\obmm-api-and-env\SKILL.md`
   - Source of truth for environment facts; updated with all locked-in decisions
   - Contains "Locked-in design decisions" section, OBMM consistency model explanation, and forbidden assumptions

- `C:\Users\zl\Desktop\ucx_obmm_br\.github\skills\uct-transport-patterns\SKILL.md`
   - Tells agent which mm/self files to mirror, which UCT framework contracts must be honored

- `C:\Users\zl\Desktop\ucx_obmm_br\.github\skills\ucx-build-verify\SKILL.md`
   - No-hardware verification commands (`ucx_info -d -t obmm`, `ucx_info -c | grep OBMM`, nm symbol checks)

- `C:\Users\zl\.copilot\session-state\b9570082-d3ac-44bc-80c0-7fcb84099c09\plan.md`
   - Current implementation plan with all 7 resolved decisions, design sections 1–8, todo breakdown
   - Sections: Discovery, NC Mapping, Memory layout, iface_addr (widened to memid+slot+pid), ep create, am_short send, iface_progress, iface_query updates, build/config wiring

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_{md,iface,ep}.{c,h}`
   - Existing skeleton — to be extended; current state has all ops as `unsupported`, caps=0, `iface_addr_t = uint64_t`
   - `obmm_iface.c:133-173` ops tables, `:175-177` `UCT_TL_DEFINE_ENTRY`, `:179` `UCT_SINGLE_TL_INIT`

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\sm\mm\base\mm_ep.c` and `mm_iface.{c,h}`
   - The reference implementation to mirror; especially `mm_ep.c:397-411` (`uct_mm_ep_am_short`) and `mm_ep.c:300-395` (common send path)

- `C:\Users\zl\Desktop\ucx_obmm_br\obmm\doc\{libobmm,obmm_set_ownership,obmm_shmdev,obmm_shmdev_sysfs}.md`
   - Authoritative obmm API + consistency model docs; the consistency model in libobmm.md lines 196-211 was the key finding that forced NC-mapping decision
</important_files>

<next_steps>
Remaining work (all of code phase):
- Rubber-duck review of the now-finalized plan (todo `plan-review` is `in_progress`, must complete before any code)
- Implement todos in dependency order: `discovery` → `region-mmap` → `pool-layout` → (`iface-addr` + `fifo-types`) → (`ep-create` + `iface-progress` + `iface-query-caps`) → `am-short-send` → `ops-table-wire` → `config-fields` → `build-verify` → `code-review`

Immediate next step:
- Call `task` tool with `agent_type: "rubber-duck"` (sync mode) to critique the locked-in plan in `plan.md`. Provide rubber-duck with:
  - Full plan.md content (especially the "Locked-in design decisions" + design sections)
  - Pointers to the obmm consistency model (libobmm.md lines 196-211) and the user's explicit guarantee about cross-node NC atomic RMW
  - Reference snippets from `mm_ep.c::uct_mm_ep_am_short` so it can compare structure
  - Ask it specifically to look for: race conditions in CAS-on-magic init, slot bitmap concurrency holes, FIFO publish ordering correctness across NC, missed UCT framework contracts (ops table / capability bits / class macros)
- After rubber-duck feedback, update plan if needed, mark `plan-review` done, set `discovery` to `in_progress`, then start coding
</next_steps>