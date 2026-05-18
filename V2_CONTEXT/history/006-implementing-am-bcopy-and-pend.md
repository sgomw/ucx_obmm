<overview>
Implementing the `obmm` UCT transport in UCX (`ucx/src/uct/obmm/`) for cross-host shared memory. v1 had only `am_short`; current segment extended it to `am_bcopy` + minimal `pending_add` so that MPI/UCP can use obmm as an AM lane. ucx_perftest at the UCT layer already passes; the active phase is now MPI integration testing through OMPI bundled in `ompi/`. Build host and run host are separate (no compilation on run host); run host has limited tooling (busybox dd, no perl/python) and cannot render Chinese, so all delivered scripts must be LF-only and English-only.
</overview>

<history>
1. User reported MPI sanity hung on cross-node, ran with `UCX_LOG=info ONLY=sanity ./run_mpi_tests.sh node1 node2`, only "RUN sanity" printed.
   - Asked user to verify mpirun launch works at all (`mpirun -np 2 -H node1,node2 hostname`).

2. User found `-H node1,node2` didn't work but `--hostfile hostfile` did. Then sanity error: `select.c:644 UCX ERROR ... obmm/memory - no am bcopy`.
   - Searched UCX source: UCP wireup `select.c:343, 1041, 1430, 1725` HARD-REQUIRES `UCT_IFACE_FLAG_AM_BCOPY` for AM lane.
   - Diagnosed: v1's am_short alone is insufficient for MPI/UCP. Asked user A/B/C choice, user picked A (implement am_bcopy).

3. User confirmed strategy: reuse same FIFO + same slot layout for am_bcopy.
   - Looked at mm transport's `uct_mm_ep_am_bcopy` and `uct_mm_ep_am_common_send` for reference.
   - mm uses separate descriptor area; ours simpler — pack_cb writes directly into `(elem+1)`.
   - Ran rubber-duck on plan. Key BLOCKING findings:
     - PENDING needed (UCP calls `ep_pending_add` after NO_RESOURCE; returning unsupported is fatal).
     - Must add `UCT_CHECK_AM_ID` in am_bcopy.
     - elem->length is uint16, must reject `FIFO_ELEM_SIZE > 65535+16`.

4. Implemented am_bcopy + minimal pending_add.
   - Modified `obmm_fifo.h`: added `FLAG_BCOPY = UCS_BIT(1)`.
   - Modified `obmm_ep.h`: added prototypes for `uct_obmm_ep_am_bcopy` and `uct_obmm_ep_pending_add`.
   - Modified `obmm_ep.c`: added `uct_obmm_ep_reserve_slot` helper (load+CAS, same as am_short); added `uct_obmm_ep_am_bcopy` and `uct_obmm_ep_pending_add` (returns `UCS_ERR_BUSY` to make UCP retry).
   - Modified `obmm_iface.c`: iface_query advertises `AM_SHORT|AM_BCOPY|PENDING|CONNECT_TO_IFACE|CB_SYNC` and sets `cap.am.max_bcopy = fifo_elem_size - 16`; iface_progress dispatches BCOPY vs short based on flag bit; iface_init rejects oversize FIFO_ELEM_SIZE; ops table wires `ep_am_bcopy` and `ep_pending_add`; added `<stdint.h>`.

5. User attempted build (succeeded). Ran cross-node sanity and got `error while loading shared libraries: libucp.so.0`.
   - Diagnosed as ssh non-interactive shell not inheriting LD_LIBRARY_PATH.
   - Modified `run_mpi_tests.sh` to add `-x PATH -x LD_LIBRARY_PATH`.

6. User reported new error: `find-available:none found` (with ompi can't find help-mca-base.txt). ompi_info shows pml:ucx and osc:ucx present.
   - Diagnosed as OMPI install prefix not propagating to remote orted.
   - Modified `run_mpi_tests.sh` to auto-derive `--prefix` from `which mpirun` and made `--mca osc ucx` opt-in via `OSC_UCX=1`.

7. User said they'll add `-x OPAL_PREFIX` themselves. After that, MPI_Init now fails with:
   `pml_ucx.c:431 ERROR: ucp_ep_create(proc=1) failed: Destination is unreachable`
   `select.c:644 UCX ERROR no active messages transport to <no debug data>: Unsupport operation`
   - Investigating wireup select.c. Found error format `"no %s transport to %s: %s"` where %s is criteria title ("active messages") and tls_info is per-TL reasons.
   - "Unsupport operation" maps to `UCS_ERR_UNSUPPORTED` but select.c only emits `UCS_ERR_UNREACHABLE` for unreachable transports — so the "Unsupport" string must come from a `ucp_wireup_check_flags`-style cap-mismatch reason per TL.
   - Was about to look more deeply at the wireup criteria when context compaction triggered.
</history>

<work_done>
Files updated this segment:

- `ucx/src/uct/obmm/base/obmm_fifo.h`: added `UCT_OBMM_FIFO_ELEM_FLAG_BCOPY = UCS_BIT(1)` to flags enum.

- `ucx/src/uct/obmm/base/obmm_ep.h`: added prototypes for `uct_obmm_ep_am_bcopy` and `uct_obmm_ep_pending_add`.

- `ucx/src/uct/obmm/base/obmm_ep.c`: 
  - Kept existing am_short with its own load+CAS slot reservation loop intact.
  - Added new static inline helper `uct_obmm_ep_reserve_slot(ep, &head)` (same load+CAS pattern).
  - Added `uct_obmm_ep_am_bcopy`: UCT_CHECK_AM_ID, reserve slot, `length = pack_cb(elem+1, arg)`, set elem fields (header=0), bus_store_fence, publish `flags = owner_bit | FLAG_BCOPY`, return `(ssize_t)length`. Ignores `flags` arg (PEER_CHECK etc.) with comment.
  - Added `uct_obmm_ep_pending_add`: returns `UCS_ERR_BUSY` (UCP retry semantics), refreshes cached_tail with bus_load_fence first.

- `ucx/src/uct/obmm/base/obmm_iface.c`:
  - iface_query: cap.flags = `AM_SHORT|AM_BCOPY|PENDING|CONNECT_TO_IFACE|CB_SYNC`; `cap.am.max_bcopy = fifo_elem_size - sizeof(elem_hdr)` (= 2032 default).
  - iface_progress: branches on `flags & FLAG_BCOPY`: BCOPY → invoke_am with `(elem+1, length)`; else (short) → invoke_am with `(&elem->header, length)`.
  - iface_init: added validation rejecting `fifo_elem_size > UINT16_MAX + sizeof(elem_hdr)`.
  - ops table: `ep_am_bcopy = uct_obmm_ep_am_bcopy`, `ep_pending_add = uct_obmm_ep_pending_add`.
  - Added `#include <stdint.h>` for UINT16_MAX.

- `run_mpi_tests.sh` (multiple revisions, all converted to LF):
  - Added `HOSTFILE=...` env var support (because `-H` doesn't work on user's setup).
  - Added `-x PATH -x LD_LIBRARY_PATH` to mpirun common opts.
  - Made `--mca osc ucx` opt-in via `OSC_UCX=1`.
  - Auto-derives `--prefix` from `which mpirun`; overridable via `OMPI_PREFIX=...`.

State:
- ✅ UCX builds successfully with all changes (user confirmed).
- ✅ Single-node MPI sanity passes (without `-H`).
- ❌ Cross-node MPI_Init fails with new wireup error: `Destination is unreachable / Unsupport operation`. Just changed from "no am bcopy" to "Unsupport operation" — UCP now tries obmm but rejects it. Root cause not yet identified.
- ❌ Other MPI tests (correctness, pingpong, bw, collective) not yet exercised.
</work_done>

<technical_details>

**Current debugging puzzle (where compaction happened)**:
- Error: `select.c:644 UCX ERROR no active messages transport to <no debug data>: Unsupport operation`
- Format string: `"no %s transport to %s: %s"` — title="active messages", peer name, tls_info (per-TL reasons joined by ", ").
- "Unsupport operation" = `ucs_status_string(UCS_ERR_UNSUPPORTED)`. But select.c:632 only writes `ucs_status_string(UCS_ERR_UNREACHABLE)` — so "Unsupport" must come from a different code path that emits UNSUPPORTED into tls_info, likely a `ucp_wireup_check_flags` failure (e.g., a remote_iface_flags mismatch). 
- The "<no debug data>" peer-name suggests the peer's worker name didn't get unpacked — could indicate addr unpack failure.
- Hypotheses to investigate:
  1. Our remote_iface_flags don't include `UCP_ADDR_IFACE_FLAG_AM_SYNC` (required at select.c:1432). This flag is derived from local CB_SYNC at address pack time — we DO advertise CB_SYNC, so should be OK. Need to verify it's properly translated when packing/unpacking address.
  2. Some other capability we still don't advertise (e.g., flush, fence semantics, get_zcopy, AM_DUP).
  3. `is_reachable_v2` returns false cross-node; "Unsupport operation" might be the printed reason from `uct_iface_fill_info_str_buf` if our impl returns wrong format.
  4. iaddr->fifo_size or geometry mismatch between nodes (different config).

**am_bcopy implementation details**:
- Receiver dispatch: BCOPY=0 → `invoke_am(am_id, &elem->header, length)` where length=8+payload_len (am_short contract: handler sees [hdr8B][payload]). BCOPY=1 → `invoke_am(am_id, elem+1, length)` where length=pack_cb_ret (no header prefix; pack_cb wrote entire buffer).
- OWNER bit (bit 0) and BCOPY bit (bit 1) coexist in single `flags` byte. Receiver only masks OWNER bit for owner check, so BCOPY bit doesn't disturb owner-bit wraparound logic.
- `elem->header = 0` for bcopy (unused but cleared for cleanliness/debugging).

**PENDING design (intentional v1 simplification)**:
- `ep_pending_add` returns `UCS_ERR_BUSY` semantically meaning "resources may be available, retry now". UCP loops: send → NO_RESOURCE → pending_add → BUSY → retry send. This is busy-wait but bounded because receiver-side iface_progress drains FIFO. Not as ordering-correct as mm-style arbiter but sufficient for v1.
- This contradicts the standard "queue and progress later" pattern. If real workloads spin too much, upgrade to mm-style `ucs_arbiter_t` queue.

**OMPI cross-node deployment gotchas (independent of UCX)**:
- ssh non-interactive non-login shell does NOT source ~/.bashrc → loses PATH/LD_LIBRARY_PATH.
  Fix: mpirun `-x PATH -x LD_LIBRARY_PATH`.
- mpirun on remote needs OMPI install prefix to find `share/openmpi/help-*.txt` files.
  Fix: `--prefix /path` or `-x OPAL_PREFIX=/path`.
- `-H node1,node2` may fail where `--hostfile` works (locale/parsing quirk on user's setup).
- `--mca osc ucx` only needed for one-sided/RMA; sanity tests don't need it.

**ucx_perftest UCT-layer worked cross-node** because it bypasses UCP entirely — directly drives UCT iface, no wireup criteria checking. So the "obmm transport" in isolation is correct; the gap is in the UCP↔UCT cap exposure layer.

**Run host constraints**:
- ARM64 Linux. No python, no perl, no compiler, no autotools.
- All scripts MUST be LF line endings (use `[IO.File]::WriteAllBytes` after `Replace("`r`n","`n")`).
- All script output MUST be English-only (run host font issue with CJK).
- bash present, coreutils limited (busybox dd, no `iflag=sync`).

**FIFO atomic constraint (resolved earlier)**:
- Cross-host slot reservation MUST use load+CAS, not FAA. FAA on full FIFO leaks slot permanently (head bumped, owner-bit never flips → in-order receiver stalls forever).
- ARM64 atomics + aligned ops are OK on this OBMM hardware (hardware team confirmed).

**Element layout**: `flags(1) + am_id(1) + length(2) + generation(4) + header(8) = 16 bytes` packed, payload immediately follows at `elem+1`. Default fifo_elem_size=2048 → 2032 bytes payload area for both short and bcopy.

**UCP wireup AM lane requirements** (ucx/src/ucp/wireup/select.c):
- Primary AM (line 343, 1430): local needs `AM_BCOPY + CONNECT_TO_IFACE`; remote needs `AM_SYNC`.
- Aux AM (line 1041): local needs `AM_BCOPY + PENDING + CONNECT_TO_IFACE` (for non-CM).
- High-bw AM (line 1725): local needs `AM_BCOPY`.
- We now advertise all of these locally. Remote AM_SYNC should derive from our CB_SYNC flag during address pack.
</technical_details>

<important_files>

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_ep.c`
  - Contains all sender-side AM logic.
  - Lines 158-226: am_short with inline load+CAS loop (pre-existing, unchanged this segment).
  - New helper `uct_obmm_ep_reserve_slot` after am_short.
  - New `uct_obmm_ep_am_bcopy` (~30 lines): pack_cb writes to (elem+1), publishes with `OWNER|BCOPY` flags.
  - New `uct_obmm_ep_pending_add`: returns UCS_ERR_BUSY, refreshes cached_tail.

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_iface.c`
  - Receive path + iface caps. Center of current debugging.
  - iface_query: now sets `AM_SHORT|AM_BCOPY|PENDING|CONNECT_TO_IFACE|CB_SYNC` and `max_bcopy = fifo_elem_size - 16`.
  - iface_progress (~lines 200-250): BCOPY-flag dispatch added.
  - iface_init (~line 270): added uint16_t length cap validation.
  - ops table (~line 345): wires ep_am_bcopy, ep_pending_add.
  - is_reachable_v2 (~lines 140-191): unchanged, but is candidate root cause for current cross-node Unsupport error.

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_fifo.h`
  - Element layout + flag bits.
  - Added `FLAG_BCOPY = UCS_BIT(1)`.

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\uct\obmm\base\obmm_ep.h`
  - Added prototypes for am_bcopy and pending_add.

- `C:\Users\zl\Desktop\ucx_obmm_br\run_mpi_tests.sh`
  - User-facing wrapper. Now supports HOSTFILE=, OSC_UCX=, OMPI_PREFIX=, ONLY=, EXTRA_OPTS=, UCX_LOG=.
  - Forwards PATH and LD_LIBRARY_PATH to remote ranks via `-x`.
  - Auto-derives --prefix.

- `C:\Users\zl\Desktop\ucx_obmm_br\ucx\src\ucp\wireup\select.c`
  - UCP wireup transport selection logic. Read-only reference.
  - Line 343: primary AM lane criteria (AM_BCOPY+CONNECT_TO_IFACE).
  - Lines 540-635: per-TL evaluation loop, builds tls_info reason string.
  - Line 644: emits the user-visible "no <title> transport to ... : <reasons>" error.
  - Lines 1420-1463: AM lane selection caller (UCP_LANE_TYPE_AM).

- `C:\Users\zl\Desktop\ucx_obmm_br\mpi_sanity.c` and other 4 MPI test sources
  - Pre-existing from prior segment. Smoke through MPI/UCP/obmm path.
  - All sizes ≤1900B (within obmm am_short cap, but now also fits am_bcopy).
</important_files>

<next_steps>
Active debugging task: cross-node MPI_Init fails with `Destination is unreachable / Unsupport operation` from `select.c:644`.

Immediate next steps:
1. Diagnose where "Unsupport operation" string is emitted into tls_info during AM lane selection. Most likely candidate: `ucp_wireup_check_flags` for our remote_iface_flags. Search ucp/wireup for code paths that write `UCS_ERR_UNSUPPORTED` into the per-TL reason buffer.
2. Verify `UCP_ADDR_IFACE_FLAG_AM_SYNC` is being set on our packed address. This flag is the remote-side requirement at select.c:1432. It's derived from local CB_SYNC during address pack — confirm in ucp/wireup/address.c.
3. Check if our `is_reachable_v2` is invoked at this stage and possibly returns false cross-node. The "<no debug data>" peer name is suspicious — could indicate `address->name` is empty due to unpack failure.
4. Ask user to run with `UCX_LOG_LEVEL=debug` and grep for `obmm` lines to see exactly what UCP sees about our transport caps (both local and remote sides).

Possible follow-up if root cause is missing remote cap:
- Add additional cap flags to iface_query (e.g., flush/fence/event_send variants).
- Check we're correctly setting `attr->cap.event_flags` if needed.

Pending todos to close once MPI passes:
- Mark in_progress todos as done.
- Run code-review agent on the diff.
- Update plan.md to reflect am_bcopy completion.
</next_steps>