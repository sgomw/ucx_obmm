---
name: vector-db-retrieval
description: >
  Query the local UCX and OBMM Chroma vector database for code-grounded
  context from ucx/src or obmm.
---

# Vector DB Retrieval

Use this skill when you need semantic retrieval from the local vector database
instead of relying only on direct file reads.

**核心原则**：代码为证，不凭记忆。所有关于 UCX 框架行为或 OBMM API 语义的结论，
必须通过本 skill 检索或直接读源文件确认，不得从记忆中推断。

## Scope

- UCX source coverage: `ucx/src`
- OBMM source coverage: `obmm`
- Persistent Chroma DB: `.artifacts/chromadb`
- Key design docs: `ucx/src/uct/obmm/docs/*.md`, `ENGINEERING_PRACTICES.md`

## Required behavior

1. Query the vector DB for relevant UCX and/or OBMM symbols or subsystems
   **before** making code-grounded conclusions.
2. Prefer vector retrieval over raw file reading whenever code context is
   needed.
3. Use a larger retrieval `k` by default so the agent sees enough surrounding
   context; for macro-heavy or ambiguous UCX code, bias toward high recall and
   widen `k` further rather than narrowing too early.
4. Prefer targeted semantic retrieval first, then inspect raw files only when
   you need exact surrounding code.
5. When the question spans both projects, retrieve from both UCX and OBMM.
6. After retrieval, **cite** the paths or symbols that drove the conclusion.

## When to invoke this skill

Invoke before any of the following actions:
- Writing or modifying a UCT transport callback (iface_ops, md_ops, ep_ops)
- Using a UCX macro whose scope or expansion is uncertain
  (e.g., `UCS_STATIC_ASSERT`, `UCT_TL_DEFINE_ENTRY`, `UCT_EP_PARAMS_CHECK_*`)
- Designing data layout that touches FIFO, pool, or region structures
- Making claims about capability flags and their downstream effect on address packing
- Writing any OBMM API call (`obmm_export`, `obmm_import`, `obmm_set_ownership`, etc.)

## Retrieval workflow

### UCX transport questions
Retrieve by callback name, struct name, macro name, or transport concept:
- `uct_mm_ep_am_short`, `uct_iface_ops_t`, `UCT_TL_COMPONENT_DEFINE`
- `uct_iface_is_reachable_v2`, `UCT_IFACE_FLAG_INTER_NODE`, `uct_base_iface_t`
- Keep `k` broad so nearby macros, typedefs, and helper code are surfaced.

### OBMM integration questions
Retrieve by API name or capability:
- `obmm_export`, `obmm_import`, `obmm_set_ownership`, `obmm_query_pa_by_memid`
- `obmm_shmdev`, `obmm_preimport`, `obmm_unimport`
- Keep `k` high enough to include adjacent ownership and error-handling details.

### OBMM UCT transport questions (combined)
When working on `ucx/src/uct/obmm/`:
1. Retrieve OBMM API semantics first (ownership model, addressing, error codes).
2. Retrieve UCX transport patterns second (iface/ep/md lifecycle, AM paths).
3. Read the relevant `ucx/src/uct/obmm/docs/*.md` design document.
4. Only then look at the implementation files.

## Known high-risk symbols (always retrieve before using)

| Symbol | Risk | What to verify |
|--------|------|----------------|
| `UCS_STATIC_ASSERT` | Statement macro, function scope only | Look at `ucs/sys/compiler_def.h` |
| `UCT_IFACE_FLAG_INTER_NODE` | Silently filters cross-node addresses | Check `ucp_worker.c` NET_ONLY filter |
| `uct_iface_scope_is_reachable` | Combines local + scope checks | Verify call ordering in is_reachable_v2 |
| `obmm_set_ownership` | 4KiB granularity; NC memory forbidden | Check `obmm/doc/obmm_set_ownership.md` |
| FIFO slot reservation | Must be load+CAS not FAA | FAA cannot roll back across hosts |

## Notes

- This skill is for semantic retrieval, not for replacing exact source review.
- If a retrieval result is still too broad, refine the query with a symbol,
  callback, or subsystem name and run another search.
- Do not skip this step when pressed for time — incorrect UCX/OBMM assumptions
  are the primary source of rework in this project.
