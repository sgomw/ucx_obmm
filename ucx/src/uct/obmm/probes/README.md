# OBMM CC ownership probe

This directory contains standalone target-side probes for the obmm UCT
transport. They are not part of the UCX build.

Build on a target node with libobmm installed:

```sh
cd ucx/src/uct/obmm/probes
make
```

Run against a cacheable CC shmdev opened without `O_SYNC`:

```sh
./obmm_cc_ownership_probe --memid <local_export_cc_memid> --iters 30
```

For manual reporting, prefer the compact mode:

```sh
./obmm_cc_ownership_probe --memid <local_export_cc_memid> --iters 30 --summary-only
```

Only copy the `SUMMARY` lines from that output.

If `UCX_OBMM_CC_MEMIDS` is set, the probe can use its first memid:

```sh
UCX_OBMM_CC_MEMIDS=<local_export_cc_memid>,<peer_import_cc_memid> \
    ./obmm_cc_ownership_probe --iters 30
```

Use `--mode write` when probing a local/exported CC region and only the sender
writeback path is needed. Use `--mode read` when probing a peer/imported CC
region and avoiding writes is important.

The output is CSV-like and intentionally short:

- `sweep` varies ownership length and dirties/touches the same length.
- `boundary` compares ranges around a 2 MiB boundary.
- `chunked` emulates the current staged zcopy shape, where a small payload can
  use a larger ownership chunk.
- `SUMMARY` lines contain the compact result to send back when logs must be
  typed manually.

Interpreting granularity:

- If 4 KiB / 64 KiB / 256 KiB release costs are close to 2 MiB, ownership
  writeback/invalidate is effectively PMD-sized even though the API accepts
  PAGE_SIZE-aligned ranges.
- If `boundary,cross_2m_8K` is close to two 2 MiB chunks, crossing a PMD
  boundary is expensive and the effective granularity is PMD-sized.
- If small ranges scale smoothly and crossing 2 MiB is not special, the
  effective cost granularity is closer to PAGE_SIZE.

If the result is PMD-sized, `CC_CHUNK_SIZE` should not be smaller than that
effective ownership granule. Smaller chunks can share one ownership granule,
which is both slower and a correctness risk when adjacent chunks are owned by
different in-flight transfers.
