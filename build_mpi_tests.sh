#!/bin/bash
# build_mpi_tests.sh -- compile MPI test binaries on the build host.
#
# Run this on the build machine. It uses mpicc from PATH (you may need to
# source the OMPI install env first, e.g.:
#     export PATH=/path/to/ompi/install/bin:$PATH
#     export LD_LIBRARY_PATH=/path/to/ompi/install/lib:$LD_LIBRARY_PATH
# )
#
# After it succeeds, scp the resulting binaries (mpi_sanity / mpi_pingpong /
# mpi_bw / mpi_correctness / mpi_collective) plus run_mpi_tests.sh to both
# nodes and run from there.

set -e

if ! command -v mpicc >/dev/null 2>&1; then
    echo "mpicc not found in PATH"
    exit 1
fi

CFLAGS="-O2 -Wall"
TARGETS="mpi_sanity mpi_pingpong mpi_bw mpi_correctness mpi_collective \
         mpi_correctness_v2 mpi_pingpong_v2 mpi_bw_v2 mpi_multi_v2"

for t in $TARGETS; do
    echo "  CC $t"
    mpicc $CFLAGS -o "$t" "${t}.c"
done

echo "OK: built: $TARGETS"