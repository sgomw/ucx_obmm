<overview>
Implementing the `obmm` UCT transport in UCX (`ucx/src/uct/obmm/`) for cross-host shared memory on the OBMM fabric. v1 only implements `am_short`. After ucx_perftest passed end-to-end (single-node self-loopback + cross-node), the user is now moving to MPI-level integration testing using the OMPI in this repo. Build machine and run machine are separate (no compilation on run machine), and run-machine cannot render Chinese (font issue), so all delivered scripts must be LF-line-ending and English-only output.
</overview>

<history>
1. User reported am_bw deadlock (am_lat passed)
   - Diagnosed: `ep_am_short` used unconditional FAA on `peer_ctl->head`, then on full returned NO_RESOURCE without rolling back. Bumped head value can never be reclaimed across hosts → in-order receiver stalls forever on the leaked slot.
   - Fixed `obmm_ep.c:174-197` by replacing FAA with load+CAS retry loop (`ucs_atomic_bool_cswap64`). NO_RESOURCE path no longer leaks slots.
   - Stored memory: "Cross-host FIFO slot reservation must use load+CAS not FAA".

2. User asked about double-import topology (each node exports + has 2 imports)
   - Walked through code: `ep_create` and `is_reachable_v2` both check export first, then imports; self-import entries exist but are unreachable in those paths.
   - Conclusion: current code already handles "self-loopback + cross-node" coexistence correctly; no need to unimport anything.

3. User reported node 0 hard-hung the entire system during 2-node am_bw
   - Initial hypothesis: `LOCK CMPXCHG` (x86) on remote NC memory not supported.
   - User clarified system is ARM64 → reinforced hypothesis: ARM64 LDXR/STXR or LSE CAS on Device memory is architecturally UNPREDICTABLE; very plausibly hung the OBMM bus.
   - Asked the user to consult hardware team.

4. User confirmed atomics & aligned ops are supported; root cause was "memory configuration", code is fine
   - User then asked about a simplified topology (node 0 export only, node 1 import only). Explained current code REQUIRES local export region for iface creation, so this asymmetric topology cannot run. Recommended minimal symmetric "each node exports + imports the other only" layout (drop self-imports).

5. User asked for a standalone atomic probe script (no UCX) to isolate
   - Wrote `obmm_atomic_probe.c` (LF, English-only) with stepwise probe: open → mmap → plain store/load → atomic store/load (RELEASE/ACQUIRE) → fetch_add → single CAS → CAS loop. Each step prints "STEP n: <name>" + flush + sleep before executing so a hang pinpoints the offending op.

6. User reports all ucx_perftest tests now pass; root cause was config
   - No code changes needed.

7. User requested MPI test cases (current request)
   - "采用当前仓库下的mpi的版本", placed at repo root, run on host directly.
   - Wrote 5 MPI test sources + build script + run wrapper + README. All LF, English-only.
</history>

<work_done>
Files created/modified across this session:

Code (committed earlier in session, still in tree):
- `ucx/src/uct/obmm/base/obmm_ep.c:174-201` — FAA→load+CAS retry loop fix for am_short slot reservation. **Verified working** (am_bw passed after fix).

Files created at repo root for MPI testing (most recent work):
- `mpi_sanity.c` — minimal MPI hello, prints rank/size/host/pid + barrier + ALL DONE.
- `mpi_pingpong.c` — 2-rank ping-pong latency. Sizes: 1, 8, 64, 256, 1024, 1900 (all under am_short cap). Default 10000 iters + 1000 warmup. Prints lat_us per size.
- `mpi_bw.c` — 2-rank one-way bandwidth. Window=64 Isend/Irecv + ack. Sizes: 64, 256, 1024, 1900. 2000 iters. Prints MB/s.
- `mpi_correctness.c` — 2-rank byte-level integrity. Deterministic pattern keyed by `pat(iter, off) = (iter*1315423911) ^ (off*2654435761)`. Sizes: 1, 7, 8, 9, 64, 255, 256, 1024, 1900. 500 iters. Aborts on mismatch. Useful for catching truncation/byte-order/stale-slot bugs.
- `mpi_collective.c` — Barrier + Bcast(64 ints) + Allreduce(64 ints, MPI_SUM). Verifies expected results.
- `build_mpi_tests.sh` — runs `mpicc -O2 -Wall` on each .c. User must source OMPI env first.
- `run_mpi_tests.sh` — wraps mpirun with `-mca pml ucx -mca osc ucx -x UCX_TLS=obmm,self -x UCX_LOG_LEVEL=${UCX_LOG:-warn}`. Args: `<node0> <node1>` or `HOSTS=node0,node1`. Supports `ONLY=name` to run a single test, `EXTRA_OPTS` for extras.
- `MPI_TESTS.md` — README explaining build/run flow, what each test checks, how to verify obmm is actually selected (look for `obmm/...` in UCX_LOG=info), env knob reference.

Pre-existing files referenced (no changes this turn):
- `obmm_atomic_probe.c` (created earlier this session, still useful)
- `obmm_probe.sh` (created earlier, no longer central)
- `C:\Users\zl\.copilot\session-state\b9570082-d3ac-44bc-80c0-7fcb84099c09\files\test-commands.md` — UCT perftest recipe (already updated to use `-d memory -x obmm`)

Status:
- ✅ ucx_perftest am_lat / am_bw / multi-pair / kill-recover all PASS (user confirmed).
- ✅ MPI test artifacts delivered, awaiting user to build on build host + run on cluster.
- ⬜ MPI tests not yet executed.
</work_done>

<technical_details>

**FIFO atomic on cross-host NC memory (resolved)**
- ARM64 LDXR/STXR and LSE CAS on Device-memory mappings are architecturally UNPREDICTABLE. We worried this caused the node-0 hang.
- User's hardware team confirmed atomics + aligned ops ARE supported on this OBMM hardware. The hang was traced to a memory configuration issue, not the UCX code. Current code (with the FAA→CAS fix) is the right design.
- Conclusion: do NOT revert to FAA, do NOT redesign to per-sender sub-FIFO. Current load+CAS approach is correct.

**FAA vs CAS fix detail (in `obmm_ep.c`)**
- Old (broken): `ucs_atomic_fadd64(&head, 1)` then check capacity post-hoc. If full, return NO_RESOURCE — but head is already bumped, leaking a slot whose owner-bit never flips. Receiver walks slots in order and stops forever at the leaked slot.
- New: `ucs_atomic_bool_cswap64(&head, head, head+1)` retry loop. Capacity check happens BEFORE committing; full → NO_RESOURCE without bumping head.
- This pattern matters for any future cross-host FIFO design in this transport.

**Topology handling**
- `obmm_md.c:174-201` (`uct_obmm_md_find_import_region` and `uct_obmm_md_export_region`) — md keeps `regions[]` with one export at `md->export_idx` and N imports (self-import + cross-node imports treated equally).
- `obmm_ep.c:64-83` and `obmm_iface.c:173-191` — both check export-self first, then walk imports[]. Self-imports work but never get used by reachability/ep_create paths because the export check wins.
- Iface creation REQUIRES a local export (`obmm_iface.c:272-277`) — asymmetric "import-only node" cannot create an iface. Symmetric "each node exports + imports the other" is the minimum supported layout.

**O_SYNC / NC mapping (resolved earlier)**
- `obmm_region.c:42`: `open(O_RDWR | O_SYNC | O_CLOEXEC)` + `mmap(...)`. Original EPERM was a memory-config issue (export flags didn't permit NC share mode), not driver O_SYNC rejection. NC mapping per obmm doc requires `O_SYNC` + (optionally) `mmap offset = 1UL<<63` for PMD; we use offset=0 (4K pages) which is also valid.

**ucx_perftest UCT-layer command form**
- Must include `-d memory -x obmm`. UCX_TLS / UCX_PROTO_INFO are UCP-only and not read by UCT perftest. `-m host` is UCP-only.

**Run-host environment constraints**
- ARM64 Linux. No python, no perl, no compiler. busybox-style dd (no sync flag).
- CJK output renders huge and clipped → all scripts MUST be English-only.
- Bash present, coreutils limited, can run pre-compiled ELF.
- Build host has full toolchain; binaries scp'd to run host.
- All shell scripts MUST be LF line endings (use `[IO.File]::WriteAllBytes` after `Replace("`r`n","`n")`).

**MPI / OMPI specifics**
- The OMPI in `ompi/` directory is the target stack. mpicc is the compile driver.
- mpirun args used: `-mca pml ucx -mca osc ucx -x UCX_TLS=obmm,self`. The `,self` is needed because UCP wireup uses self lane for control.
- All MPI test sizes capped at ≤1900B because we only implement am_short. Going larger forces UCP to bcopy/zcopy/rndv which we don't support; with `UCX_TLS=obmm,self` it will fail rather than silently fall back.
- `mpi_correctness.c` byte pattern: `pat(iter, off) = (uint8_t)((iter*1315423911u) ^ (off*2654435761u))` — both prime constants per Knuth multiplicative hash convention, ensures fast cross-byte decorrelation.

**TODO state (in SQL)**
- 6 done, 6 in_progress, 2 pending. Build-verify and code-review are still pending. Most "in_progress" items (iface-addr, ep-create, am-short-send, iface-progress, iface-query-caps, ops-table-wire) are actually code-complete and runtime-verified now via ucx_perftest passing.
</technical_details>

<important_files>
- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_ep.c`
   - Contains the am_short send path. Lines 174-201 hold the load+CAS slot reservation loop (the deadlock fix).
   - Cited by stored memory "FIFO slot reservation".

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_iface.c`
   - Receive path `uct_obmm_iface_progress` lines 194-244 (in-order owner-bit walk + tail update).
   - `is_reachable_v2` lines 140-191 (export-then-import lookup).
   - Iface init lines 247-320 (requires local export region).

- `C:\Users\zl\Desktop\ucx_obmm_br\mpi_sanity.c`, `mpi_pingpong.c`, `mpi_bw.c`, `mpi_correctness.c`, `mpi_collective.c`
   - The 5 MPI test programs the user will compile on build host and scp to run host.
   - All LF, English-only output, sizes ≤1900B.

- `C:\Users\zl\Desktop\ucx_obmm_br\build_mpi_tests.sh`
   - Compiles all 5 with `mpicc -O2 -Wall`. User sources OMPI env vars first.

- `C:\Users\zl\Desktop\ucx_obmm_br\run_mpi_tests.sh`
   - Runs all 5 with mpirun + `-mca pml ucx -mca osc ucx -x UCX_TLS=obmm,self`. Supports `HOSTS=`, `ONLY=`, `UCX_LOG=`, `EXTRA_OPTS=`.

- `C:\Users\zl\Desktop\ucx_obmm_br\MPI_TESTS.md`
   - User-facing recipe for build + run workflow, env knobs, verification checklist.

- `C:\Users\zl\Desktop\ucx_obmm_br\obmm_atomic_probe.c`
   - Standalone probe (no UCX). Still useful for future hardware/config triage.

- `C:\Users\zl\.copilot\session-state\b9570082-d3ac-44bc-80c0-7fcb84099c09\files\test-commands.md`
   - The ucx_perftest recipe (UCT-layer commands, `-d memory -x obmm`). Already updated and verified to work.

- `C:\Users\zl\.copilot\session-state\b9570082-d3ac-44bc-80c0-7fcb84099c09\plan.md`
   - Implementation plan. Should be reviewed/updated to reflect: ucx_perftest fully passing, MPI testing now in progress.
</important_files>

<next_steps>
Immediate (waiting on user):
- User runs `build_mpi_tests.sh` on build host (after sourcing OMPI env), scp's 5 binaries + `run_mpi_tests.sh` to both nodes.
- User runs `./run_mpi_tests.sh node0 node1` (or with `ONLY=sanity` first to smoke-test). Expected order: sanity → correctness → pingpong → bw → collective.

If MPI tests fail or hang:
- First check `UCX_LOG=info ./run_mpi_tests.sh ...` and look for `obmm/...` lane selection. If UCX_TLS filter rejects everything, may need to add `,sm` to UCX_TLS for intra-node fallback (currently we don't, to force obmm).
- mpi_correctness mismatches → likely am_short truncation, generation drift, or stale slot bug. Map iter/off back to FIFO slot.
- mpi_bw hang (not crash) → recheck CAS path; mpi_bw injects more concurrent inflight than ucx_perftest's fixed window so could expose new races.

Pending todos to close after MPI passes:
- Mark in_progress todos as done (iface-addr, ep-create, am-short-send, iface-progress, iface-query-caps, ops-table-wire all verified by passing ucx_perftest + MPI).
- Pending: build-verify (essentially done), code-review (run code-review agent on the diff).
- Update plan.md to reflect current state.
</next_steps>