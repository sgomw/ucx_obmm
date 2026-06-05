# OBMM CC probes

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
- `chunked` emulates the current staged zcopy shape: each UCT fragment carries
  an estimated AM header (`--header-size`, default 4 KiB), and ownership is
  rounded to `--own-granule` (default 2 MiB). Chunk sizes below the ownership
  granule are reported as `-1` in the summary because the transport rejects
  that geometry.
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

## Sender-owned vs receiver-owned CC path probe

Use `obmm_cc_owned_path_probe` on two target nodes to compare the previous
sender-owned staged path with the receiver-owned layout. TCP is used
only for synchronization and timing exchange; payload bytes move only through
the CC shmdev mappings.

Run role B first:

```sh
./obmm_cc_owned_path_probe --role b --listen 0.0.0.0:19999 \
    --local-export-memid <B_export_cc_memid> \
    --peer-import-memid <A_export_imported_on_B_cc_memid> \
    --summary-only
```

Then run role A:

```sh
./obmm_cc_owned_path_probe --role a --connect <B_ip>:19999 \
    --local-export-memid <A_export_cc_memid> \
    --peer-import-memid <B_export_imported_on_A_cc_memid> \
    --summary-only
```

Only role A prints `SUMMARY` lines. Copy those lines back.

Path meanings:

- `sender_total_us`: A writes A local/exported CC, then B reads A through B's
  peer/imported CC mapping. This models the previous sender-owned staged
  `AM_ZCOPY` path.
- `receiver_total_us`: A writes B through A's peer/imported CC mapping, then B
  reads B local/exported CC. This models the proposed receiver-owned layout.
- `receiver_ratio`: `receiver_total_us / sender_total_us`; values below 1.0
  mean receiver-owned is faster for that size.

Defaults are tuned to the current staged AM shape:

- payload sizes: `256K,512K,1M,2M,4M`
- `--header-size 4K`
- `--own-granule 2M`
- `--map-size 16M`
- `--iters 20 --warmup 3`

If you need to reduce manual output, keep `--summary-only`. If you need raw
writer/read split details, omit it; role A then prints one CSV row per
path/size plus the same `SUMMARY` lines.

## Receiver-owned batch/epoch CC path probe

Use `obmm_cc_batch_path_probe` on two target nodes to test whether a
receiver-owned CC epoch can amortize ownership transitions across multiple
large messages. TCP is used only for synchronization and timing exchange;
payload bytes move only through the receiver's CC region.

Run role B first:

```sh
./obmm_cc_batch_path_probe --role b --listen 0.0.0.0:19999 \
    --local-export-memid <B_export_cc_memid> \
    --summary-only
```

Then run role A:

```sh
./obmm_cc_batch_path_probe --role a --connect <B_ip>:19999 \
    --peer-import-memid <B_export_imported_on_A_cc_memid> \
    --summary-only
```

Only role A prints `SUMMARY` lines. Copy those lines back.

Defaults are intentionally compact:

- payload sizes: `2M,4M`
- batch counts: `1,2,4,8`
- `--header-size 4K`
- `--own-granule 2M`
- `--map-size 128M`
- `--iters 10 --warmup 2`

Path meaning:

- `batch=1` is the current receiver-owned per-message ownership shape.
- `batch>1` flips one contiguous CC range for multiple messages, then reports
  costs divided by the number of messages.
- `speedup` is relative to the first batch count tested for the same size.

The key fields to report are:

- `SUMMARY size=... batch=... per_msg_us=... speedup=...`
- `writer_own_per_msg_us` and `reader_own_per_msg_us`
- `writer_copy_per_msg_us` and `reader_copy_per_msg_us`

If `per_msg_us` and ownership-per-message drop sharply as `batch` increases,
the next viable protocol shape is a receiver-owned bulk epoch. If copy time
dominates or `speedup` stays near 1, CC ownership batching is unlikely to save
the high-concurrency OSU large-message path.
