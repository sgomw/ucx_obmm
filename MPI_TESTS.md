# OBMM V3 MPI Test Recipe

End-to-end tests of the obmm UCT transport through OMPI/UCP.

## What each test does

| test | ranks | what it checks |
| --- | ---: | --- |
| `mpi_sanity` | 2 | MPI starts, ranks see each other |
| `mpi_correctness_v3` | 2 | bidirectional byte correctness across short, bcopy, 16 KiB CC chunk boundary, and fragmented sizes |
| `mpi_pressure_v3` | 2 | sustained bidirectional pressure; default 32 KiB x window 64 to stress pending and CC chunk reclaim |
| `mpi_multi_v3` | N | ring plus alltoall correctness/load coverage for larger process counts |

## Build

```bash
export PATH=/path/to/ompi/install/bin:$PATH
export LD_LIBRARY_PATH=/path/to/ompi/install/lib:$LD_LIBRARY_PATH

./build_mpi_tests.sh
```

Copy `mpi_sanity`, `mpi_correctness_v3`, `mpi_pressure_v3`,
`mpi_multi_v3`, and `run_mpi_tests_v3.sh` to both nodes.

## Run

```bash
UCX_OBMM_MEM_MODE=hybrid UCX_OBMM_NC_MEMIDS=1,2 UCX_OBMM_CC_MEMIDS=3,4 \
  STAGE=smoke ./run_mpi_tests_v3.sh node0 node1

UCX_OBMM_MEM_MODE=hybrid UCX_OBMM_NC_MEMIDS=1,2 UCX_OBMM_CC_MEMIDS=3,4 \
  STAGE=all MULTI_NP=16 ./run_mpi_tests_v3.sh node0 node1
```

For NC regression plus hybrid coverage:

```bash
MODE=both UCX_OBMM_NC_MEMIDS=1,2 UCX_OBMM_CC_MEMIDS=3,4 \
  STAGE=all MULTI_NP=16 ./run_mpi_tests_v3.sh node0 node1
```

## Common knobs

| env | effect |
| --- | --- |
| `UCX_OBMM_MEM_MODE=nc|hybrid` | select V3 mode |
| `UCX_OBMM_NC_MEMIDS` | mandatory NC memid CSV |
| `UCX_OBMM_CC_MEMIDS` | mandatory for hybrid |
| `STAGE=smoke|boundary|pressure|multi|all` | choose test stage |
| `UCX_LOG=info` | UCX logs for transport selection and wireup |
| `MULTI_NP=16` | process count for `mpi_multi_v3` |
