# OBMM MPI Test Recipe

End-to-end tests of the obmm UCT transport via OMPI (the MPI in this repo).

## What each test does

| test                | ranks | what it checks                                          |
|---------------------|-------|---------------------------------------------------------|
| mpi_sanity          | 2     | MPI starts, ranks see each other                        |
| mpi_correctness     | 2     | byte-level integrity at sizes 1..1900 (am_short range)  |
| mpi_pingpong        | 2     | latency at 1, 8, 64, 256, 1024, 1900 bytes              |
| mpi_bw              | 2     | one-way bandwidth at 64, 256, 1024, 1900 bytes          |
| mpi_collective      | 2     | Barrier / Bcast / Allreduce sanity                      |
| mpi_correctness_v2  | 2     | byte-level integrity 1..1 MiB; hits short/bcopy/frag boundaries; WINDOW=16 |
| mpi_pingpong_v2     | 2     | latency 1..1 MiB; reads off the protocol-transition steps |
| mpi_bw_v2           | 2     | one-way BW 64..1 MiB; exercises am_bcopy + UCP fragmentation |
| mpi_multi_v2        | N     | ring + alltoall, sizes 1..64 KiB, validates >2 ranks   |

v1 tests stay under am_short cap; v2 tests cross am_short→am_bcopy→
fragmentation boundaries to validate the v2 desc-paired bcopy path.

## Build (build host)

```bash
# Make sure mpicc points at the OMPI you built.
export PATH=/path/to/your/ompi/install/bin:$PATH
export LD_LIBRARY_PATH=/path/to/your/ompi/install/lib:$LD_LIBRARY_PATH

./build_mpi_tests.sh
```

Outputs: `mpi_sanity mpi_pingpong mpi_bw mpi_correctness mpi_collective
mpi_correctness_v2 mpi_pingpong_v2 mpi_bw_v2`.

scp those + `run_mpi_tests.sh` to BOTH nodes (same path on both).

## Run (run host -- either node)

```bash
# Make sure UCX (with obmm) and OMPI are reachable.
export PATH=/path/to/ompi/install/bin:$PATH
export LD_LIBRARY_PATH=/path/to/ompi/install/lib:/path/to/ucx/install/lib:$LD_LIBRARY_PATH

./run_mpi_tests.sh node0 node1
```

To pin a single test:
```bash
ONLY=pingpong ./run_mpi_tests.sh node0 node1
```

To run only the v2 suite:
```bash
ONLY=v2 ./run_mpi_tests.sh node0 node1
```

To get UCX info logs:
```bash
UCX_LOG=info ./run_mpi_tests.sh node0 node1
```

## Verifying obmm is actually used

Run any test with `UCX_LOG=info` and look for lines like:
- `obmm/...` selected as a transport for the AM lane
- No `tcp/...` or `rdma/...` for AM (we forced `UCX_TLS=obmm,self`)

If you see `unsupported transport`, OMPI was not built against this UCX,
or `UCX_TLS` filtered out everything.

## Common knobs (env)

| env                  | effect                                                       |
|----------------------|--------------------------------------------------------------|
| UCX_TLS=obmm,self    | force obmm, leave self for wireup (already in run script)    |
| UCX_LOG_LEVEL=info   | UCX info logs (transport selection, wireup)                  |
| UCX_PROTO_INFO=y     | print the protocol UCP picks for each operation              |
| OMPI_MCA_pml=ucx     | force UCX PML (already in run script)                        |
| OMPI_MCA_osc=ucx     | force UCX OSC for one-sided                                  |