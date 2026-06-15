# OBMM NC Local Memory Probe

`obmm_nc_mem_probe.c` is a standalone Linux target probe for isolating local
NC mmap bandwidth from UCP/MPI protocol effects. It does not link UCX or
libobmm. It opens `/dev/obmm_shmdev<MEMID>` with `O_SYNC`, maps it as NC, and
copies between normal DRAM and the mapped region.

Build on the target node:

```sh
gcc -O3 -Wall -Wextra -o obmm_nc_mem_probe obmm_nc_mem_probe.c
```

Run only when the selected NC export is not being used by UCX. The probe writes
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
