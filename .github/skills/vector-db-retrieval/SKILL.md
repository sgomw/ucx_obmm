---
name: vector-db-retrieval
description: >
  Query the local UCX and OBMM Chroma vector database for code-grounded
  context from ucx/src or obmm.
---

# Vector DB Retrieval

Use this skill when you need semantic retrieval from the local vector database
instead of relying only on direct file reads.

## Scope

- UCX source coverage: `ucx\src`
- OBMM source coverage: `obmm`
- Persistent Chroma DB: `.\.artifacts\chromadb`

## Required behavior

1. Query the vector DB for relevant UCX and/or OBMM symbols or subsystems
   before making code-grounded conclusions.
2. Prefer vector retrieval over raw file reading whenever code context is
   needed.
3. Use a larger retrieval `k` by default so the agent sees enough surrounding
   context; for macro-heavy or ambiguous UCX code, bias toward high recall and
   widen `k` further rather than narrowing too early.
4. Prefer targeted semantic retrieval first, then inspect raw files only when
   you need exact surrounding code.
5. When the question spans both projects, retrieve from both UCX and OBMM.

## Retrieval workflow

1. For UCX transport questions, retrieve by:
   - callback name, struct name, macro name, or transport concept
   - examples: `uct_mm_ep_am_short`, `uct_iface_ops_t`, `UCT_TL_COMPONENT_DEFINE`
   - keep `k` broad so nearby macros, typedefs, and helper code are surfaced

2. For OBMM integration questions, retrieve by:
   - API name or capability
   - examples: `obmm_export`, `obmm_import`, `obmm_query_pa_by_memid`, `obmm_set_ownership`
   - keep `k` high enough to include adjacent implementation details

3. When producing design output, combine the relevant UCX and OBMM hits.

## Expected output style

- Cite the retrieved paths or symbols that drove the conclusion.

## Notes

- This skill is for semantic retrieval, not for replacing exact source review.
- If a retrieval result is still too broad, refine the query with a symbol,
  callback, or subsystem name and run another search.
