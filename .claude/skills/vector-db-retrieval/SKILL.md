---
name: vector-db-retrieval
description: >
  Query the local UCX, OBMM, and OMPI Chroma vector database for code-grounded
  context from the ucx, obmm, and ompi repositories.
---

# Vector DB Retrieval

Use this skill when you need semantic retrieval from the local vector database
instead of relying only on direct file reads.

## Scope

- Persistent Chroma DB: `.\.artifacts\chromadb`
- UCX collection: `ucx_code` (repo root `ucx`, excluding `ucx\src\uct\obmm\`)
- OBMM collection: `obmm_code` (repo root `obmm`)
- OMPI collection: `ompi_code` (repo root `ompi`)
- Useful metadata on each hit: `path`, `root`, `start_line`, `end_line`
- Helper query script: `.\.claude\skills\vector-db-retrieval\query_chroma.py`

## Current DB caveat

- The persisted collections are 1024-dimensional.
- This workspace does not store the embedding model identity alongside the
  collections.
- Do **not** query this DB with Chroma's `DefaultEmbeddingFunction`; the
  default local embedding is 384-dimensional and raises a dimension mismatch.
- Until a matching 1024-dimensional embedding model is explicitly documented
  and wired up, prefer the helper script's SQLite FTS retrieval plus path bias
  over raw `collection.query()` calls.

## Required behavior

1. Query the relevant collection (`ucx_code`, `obmm_code`, and/or
   `ompi_code`) before making code-grounded conclusions.
2. In this workspace, satisfy that requirement by running
   `python .\.claude\skills\vector-db-retrieval\query_chroma.py ...` unless a
   matching 1024-dimensional embedding function has been configured.
3. Prefer the helper script's retrieval over raw file reading whenever code
   context is needed.
4. Use a larger retrieval `k` by default so the agent sees enough surrounding
   context; for macro-heavy or ambiguous UCX code, bias toward high recall and
   widen `k` further rather than narrowing too early.
5. Because `ucx_code` covers the repo root rather than only `ucx\src` and
   intentionally excludes `ucx\src\uct\obmm\`, use the
   returned `path` metadata to bias toward code paths first; for UCX transport
   work, prefer hits under `ucx\src\`, especially `ucx\src\uct\` and
   `ucx\src\ucp\`.
6. For OBMM questions, prefer hits under `obmm\src\libobmm\` and `obmm\doc\`;
   de-prioritize license or other repository-noise matches unless the prompt is
   explicitly about them.
7. For OMPI questions, prefer hits under `ompi\ompi\`, `ompi\opal\`, and
   `ompi\oshmem\`; de-prioritize generated config files or bundled third-party
   code unless the prompt is explicitly about them.
8. Prefer targeted retrieval first, then inspect raw files only when
   you need exact surrounding code.
9. When the question spans multiple projects, retrieve from each relevant
   collection.

## Retrieval workflow

1. For UCX transport questions, retrieve by:
    - callback name, struct name, macro name, or transport concept
    - examples: `uct_mm_ep_am_short`, `uct_iface_ops_t`, `UCT_TL_COMPONENT_DEFINE`
    - preferred invocation:
      `python .\.claude\skills\vector-db-retrieval\query_chroma.py "uct_mm_ep_am_short" --collection ucx_code --k 30`
    - keep `k` broad so nearby macros, typedefs, and helper code are surfaced
    - if retrieval is noisy, refine with a path cue such as `ucx/src/uct/obmm`
      or `ucx/src/uct/base`

2. For OBMM integration questions, retrieve by:
    - API name or capability
    - examples: `obmm_export`, `obmm_import`, `obmm_query_pa_by_memid`, `obmm_set_ownership`
    - preferred invocation:
      `python .\.claude\skills\vector-db-retrieval\query_chroma.py "obmm_export" --collection obmm_code --k 10`
    - keep `k` high enough to include adjacent implementation details
    - if retrieval is noisy, refine with a path cue such as
      `obmm/src/libobmm` or `obmm/doc`

3. For OMPI runtime or MPI questions, retrieve by:
    - MPI API name, OPAL/OMPI/OSHMEM subsystem, datatype, communicator, or
      runtime concept
    - examples: `MPI_Init`, `ompi_comm_rank`, `opal_free_list_t`,
      `mca_btl_base_select`
    - preferred invocation:
      `python .\.claude\skills\vector-db-retrieval\query_chroma.py "MPI_Init" --collection ompi_code --k 20`
    - keep `k` high enough to surface nearby framework glue and component
      registration code
    - if retrieval is noisy, refine with a path cue such as `ompi/ompi`,
      `ompi/opal`, or `ompi/oshmem`

4. When producing design output, combine the relevant UCX, OBMM, and OMPI hits.

## Expected output style

- Cite the collection plus the retrieved path or symbol that drove the
  conclusion.

## Notes

- This skill is for semantic retrieval, not for replacing exact source review.
- If a retrieval result is still too broad, refine the query with a symbol,
  callback, subsystem name, or path hint and run another search.
- The helper script intentionally applies repository-specific path bias so code
  directories outrank repo noise such as CI, config, or top-level dotfiles.
