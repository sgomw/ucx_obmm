---
name: vector-db-retrieval
description: >
  Query the local UCX and OBMM Chroma vector database for code-grounded
  context from the ucx and obmm repositories.
---

# Vector DB Retrieval

Use this skill when you need semantic retrieval from the local vector database
instead of relying only on direct file reads.

## Scope

- Persistent Chroma DB: `.\.artifacts\chromadb`
- UCX collection: `ucx_code` (repo root `ucx`)
- OBMM collection: `obmm_code` (repo root `obmm`)
- Useful metadata on each hit: `path`, `root`, `start_line`, `end_line`

## Required behavior

1. Query the relevant collection (`ucx_code` and/or `obmm_code`) before making
   code-grounded conclusions.
2. Prefer vector retrieval over raw file reading whenever code context is
   needed.
3. Use a larger retrieval `k` by default so the agent sees enough surrounding
   context; for macro-heavy or ambiguous UCX code, bias toward high recall and
   widen `k` further rather than narrowing too early.
4. Because `ucx_code` covers the repo root rather than only `ucx\src`, use the
   returned `path` metadata to bias toward code paths first; for UCX transport
   work, prefer hits under `ucx\src\`, especially `ucx\src\uct\` and
   `ucx\src\ucp\`.
5. For OBMM questions, prefer hits under `obmm\src\libobmm\` and `obmm\doc\`;
   de-prioritize license or other repository-noise matches unless the prompt is
   explicitly about them.
6. Prefer targeted semantic retrieval first, then inspect raw files only when
   you need exact surrounding code.
7. When the question spans both projects, retrieve from both collections.

## Retrieval workflow

1. For UCX transport questions, retrieve by:
    - callback name, struct name, macro name, or transport concept
    - examples: `uct_mm_ep_am_short`, `uct_iface_ops_t`, `UCT_TL_COMPONENT_DEFINE`
    - keep `k` broad so nearby macros, typedefs, and helper code are surfaced
    - if retrieval is noisy, refine with a path cue such as `ucx/src/uct/obmm`
      or `ucx/src/uct/base`

2. For OBMM integration questions, retrieve by:
    - API name or capability
    - examples: `obmm_export`, `obmm_import`, `obmm_query_pa_by_memid`, `obmm_set_ownership`
    - keep `k` high enough to include adjacent implementation details
    - if retrieval is noisy, refine with a path cue such as
      `obmm/src/libobmm` or `obmm/doc`

3. When producing design output, combine the relevant UCX and OBMM hits.

## Expected output style

- Cite the collection plus the retrieved path or symbol that drove the
  conclusion.

## Notes

- This skill is for semantic retrieval, not for replacing exact source review.
- If a retrieval result is still too broad, refine the query with a symbol,
  callback, subsystem name, or path hint and run another search.
