# OBMM Target Probes

`obmm_nc_mem_probe.c` is a standalone Linux target probe for isolating local
NC mmap bandwidth from UCP/MPI protocol effects. It does not link UCX or
libobmm. It opens `/dev/obmm_shmdev<MEMID>` with `O_SYNC`, maps it as NC, and
copies between normal DRAM and the mapped region.

`obmm_alias_probe.c` is a standalone Linux target probe for the single-export
dual-mmap design question. It opens the same `/dev/obmm_shmdev<MEMID>` twice:
once cacheable without `O_SYNC`, and once non-cacheable with `O_SYNC`. It then
tests whether the two aliases can safely be used for same-node CC traffic and
cross-node-like NC traffic.

Build on the target node:

```sh
gcc -O3 -Wall -Wextra -o obmm_nc_mem_probe obmm_nc_mem_probe.c
gcc -O3 -Wall -Wextra -o obmm_alias_probe obmm_alias_probe.c
```

Run only when the selected export is not being used by UCX. Both probes write
into the mapped region.

Useful local-NC wall tests:

```sh
# FIFO-like producer/consumer handoff: writer copies payload to NC, publishes a
# flag, then reader copies the payload from NC after observing the flag.
mpirun -np 70 --map-by slot ./obmm_nc_mem_probe \
    --memid "$NC_EXPORT_MEMID" --mode handoff --bytes 4194304 --seconds 5

# Unsynchronized same-address pressure. Use this only to test whether local NC
# reads collapse when writers hammer the same region concurrently.
mpirun -np 70 --map-by slot ./obmm_nc_mem_probe \
    --memid "$NC_EXPORT_MEMID" --mode pair --bytes 4194304 --seconds 5

# Separate producer and consumer ceilings.
mpirun -np 70 --map-by slot ./obmm_nc_mem_probe \
    --memid "$NC_EXPORT_MEMID" --mode write --bytes 4194304 --seconds 5

mpirun -np 70 --map-by slot ./obmm_nc_mem_probe \
    --memid "$NC_EXPORT_MEMID" --mode read --bytes 4194304 --seconds 5
```

Each rank prints one `OBMM_NC_MEM_PROBE` line; the probe does not print an
extra summary line.

Compare externally summed per-node `bw_GiBs` from `mode=handoff` with OSU local
NC effective traffic. If they land on the same plateau, the large-message wall
is local NC mmap payload movement, not UCP protocol selection. If
unsynchronized `pair` readers collapse but `handoff` readers do not, the
collapse is same-address read/write contention in the probe, not the OSU data
path.

## Dual-Alias Tests

Start with a single-process alias visibility test. This checks raw CC->NC and
NC->CC readback in one process. The probe maps only the test window at offset 0
by default, because some kernels or drivers may reject two full 3 GiB mappings
in one process even when a small dual-alias window is legal.

```sh
./obmm_alias_probe --memid "$EXPORT_MEMID" --mode alias \
    --bytes 4096 --iters 10000
```

If the second mmap still fails, reverse the order to distinguish "NC after CC"
from a general dual-alias limitation:

```sh
./obmm_alias_probe --memid "$EXPORT_MEMID" --mode alias \
    --bytes 4096 --iters 10000 --map-order nc-first
```

Use `--full-map` only as a diagnostic for whether two complete-region mappings
are allowed:

```sh
./obmm_alias_probe --memid "$EXPORT_MEMID" --mode alias \
    --bytes 4096 --iters 10000 --full-map
```

Then run same-node two-process handoff tests. The probe uses MPI/PMI/SLURM
rank environment variables if present; otherwise set `OBMM_PROBE_LOCAL_RANK`
and `OBMM_PROBE_LOCAL_SIZE` manually.

```sh
# Pure cacheable path: writer uses CC alias, reader uses CC alias.
mpirun -np 2 --map-by slot ./obmm_alias_probe \
    --memid "$EXPORT_MEMID" --mode cc-handoff --bytes 4096 --iters 10000

# Pure non-cacheable path on the local export: writer uses NC alias, reader
# uses NC alias. This approximates the local side of the cross-node NC FIFO.
mpirun -np 2 --map-by slot ./obmm_alias_probe \
    --memid "$EXPORT_MEMID" --mode nc-handoff --bytes 4096 --iters 10000

# Risky design: one physical control/data record alternates between CC and NC
# aliases. This is the closest probe for a single mixed FIFO.
mpirun -np 2 --map-by slot ./obmm_alias_probe \
    --memid "$EXPORT_MEMID" --mode mixed-ctl --bytes 4096 --iters 10000

# Conservative design: CC and NC use separate physical records. This models
# split control/FIFO regions inside one exported OBMM region.
mpirun -np 2 --map-by slot ./obmm_alias_probe \
    --memid "$EXPORT_MEMID" --mode split-ctl --bytes 4096 --iters 10000
```

Each rank prints one `OBMM_ALIAS_PROBE` line. A `status=PASS` on
`split-ctl` but not `mixed-ctl` means a single export can still be viable, but
the transport should keep CC and NC control/FIFO cachelines physically
separate. A `status=FAIL` or `timeouts>0` on `cc-handoff` would reject the
same-node CC alias path for this design.
