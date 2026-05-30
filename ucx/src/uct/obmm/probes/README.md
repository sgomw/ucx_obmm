# OBMM CC probes

These are standalone Linux probes for the CC-only OBMM redesign. They do not
link into UCX and do not use the legacy NC transport data path.

The probes open `/dev/obmm_shmdev<memid>` with `O_RDWR` only. They never set
`O_SYNC`, never export or import memory, and never call the OBMM lifecycle APIs.

## Build

On the OBMM Linux host:

```sh
cd ucx/src/uct/obmm/probes
make
```

The build does not require libobmm headers or `libobmm.so`. The probe resolves
`obmm_set_ownership` with `dlopen()` at runtime on the execution host. If
`libobmm.so` is not in the runtime loader path:

```sh
export OBMM_LIBOBMM_PATH=/path/to/libobmm.so
```

## Discover devices

```sh
./obmm_cc_probe list
```

Use the export memid on the writer host and the matching import memid on the
reader host. The offset and length must describe the same page range in that
export/import pair.

## Single-process probes

Permission flip latency with a persistent `PROT_NONE` mapping:

```sh
./obmm_cc_probe flip --memid 1 --length 4096 --iters 100000 --prot write
./obmm_cc_probe flip --memid 1 --length 4096 --iters 100000 --prot read
```

Dynamic `mmap()/munmap()` latency:

```sh
./obmm_cc_probe flip --memid 1 --mode mmap --length 4096 --iters 100000 --prot write
```

Local access bandwidth while repeatedly acquiring and releasing:

```sh
./obmm_cc_probe touch --memid 1 --op write --length 1048576 --iters 1000
./obmm_cc_probe touch --memid 1 --op read  --length 1048576 --iters 1000
```

## Two-node handoff

Reader node, mapping the import shmdev that mirrors the writer's export:

```sh
./obmm_cc_probe handoff --role reader --listen 29001 \
    --memid <reader-import-memid> --offset 0 --length 1048576 \
    --iters 1000 --mode ownership --verify
```

Writer node, mapping its local export shmdev:

```sh
./obmm_cc_probe handoff --role writer --connect <reader-host>:29001 \
    --memid <writer-export-memid> --offset 0 --length 1048576 \
    --iters 1000 --mode ownership --verify
```

Sweep `--length` from one page through multi-MiB chunks. Repeat with
`--mode mmap` to compare persistent mapping plus `obmm_set_ownership()` against
dynamic `mmap()/munmap()` ownership.

Fence policy is selectable:

```sh
--fence none
--fence seq_cst
--fence arm_ish
--fence arm_osh
```

The default is `seq_cst`. Use verification runs to detect stale-data behavior
before using a faster fence setting for performance sweeps.
