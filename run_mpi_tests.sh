#!/bin/bash
# run_mpi_tests.sh -- run obmm-only MPI tests on 2 nodes.
#
# Usage:
#     ./run_mpi_tests.sh <node0_host> <node1_host>
# or set HOSTS env:
#     HOSTS=node0,node1 ./run_mpi_tests.sh
#
# Optional env:
#     MPIRUN     - override mpirun command (default: mpirun)
#     EXTRA_OPTS - extra mpirun args
#     ONLY       - run a single test by name (sanity|pingpong|bw|correctness|collective|*_v2)
#                  or "v2" to run only the v2 suite
#     UCX_LOG    - UCX_LOG_LEVEL (default: warn). Set to info or debug for triage.
#     MULTI_NP   - np for mpi_multi_v2 (default: 8). Try 8, 16, 32, 39, 40 to stress.
#
# Tests run in this order:
#   1) mpi_sanity         -- can MPI start at all
#   2) mpi_correctness    -- byte-level correctness (am_short range)
#   3) mpi_pingpong       -- latency (am_short range)
#   4) mpi_bw             -- bandwidth (am_short range)
#   5) mpi_collective     -- collectives sanity
#   6) mpi_correctness_v2 -- byte-level correctness across short/bcopy/frag (np=2)
#   7) mpi_pingpong_v2    -- latency across short/bcopy/frag (np=2)
#   8) mpi_bw_v2          -- bandwidth across short/bcopy/frag (np=2)
#   9) mpi_multi_v2       -- N-rank ring + alltoall (np=$MULTI_NP)

set -e

if [ -n "$1" ] && [ -n "$2" ]; then
    HOSTS="$1,$2"
fi
if [ -z "$HOSTS" ] && [ -z "$HOSTFILE" ]; then
    echo "usage: $0 <node0_host> <node1_host>"
    echo "   or: HOSTS=node0,node1 $0"
    echo "   or: HOSTFILE=hostfile $0"
    exit 1
fi

if [ -n "$HOSTFILE" ]; then
    HOST_OPT="--hostfile ${HOSTFILE}"
else
    HOST_OPT="-H ${HOSTS}"
fi

MPIRUN="${MPIRUN:-mpirun}"
UCX_LOG="${UCX_LOG:-warn}"
EXTRA_OPTS="${EXTRA_OPTS:-}"
MULTI_NP="${MULTI_NP:-8}"

# Build COMMON_OPTS WITHOUT -np; each run_one supplies its own.
COMMON_OPTS="\
    ${HOST_OPT} \
    --map-by node \
    --mca pml ucx \
    --allow-run-as-root \
    -x OPAL_PREFIX \
    -x PATH \
    -x LD_LIBRARY_PATH \
    -x UCX_TLS=obmm,self \
    -x UCX_LOG_LEVEL=${UCX_LOG} \
    ${EXTRA_OPTS}"

run_one() {
    local name="$1"
    local args="$2"
    local np="${3:-2}"
    local bin="./mpi_${name}"

    if [ -n "$ONLY" ] && [ "$ONLY" != "$name" ] \
       && ! { [ "$ONLY" = "v2" ] && [ "${name%_v2}" != "$name" ]; }; then
        return 0
    fi
    if [ ! -x "$bin" ]; then
        echo "SKIP ${name}: ${bin} not found / not executable"
        return 0
    fi

    echo "=========================================="
    echo "RUN  ${name}  (np=${np})"
    echo "=========================================="
    ${MPIRUN} -np ${np} ${COMMON_OPTS} ${bin} ${args}
    echo
}

run_one sanity         ""
run_one correctness    ""
run_one pingpong       ""
run_one bw             ""
run_one collective     ""
run_one correctness_v2 ""
run_one pingpong_v2    ""
run_one bw_v2          ""
run_one multi_v2       "" "${MULTI_NP}"

echo "ALL DONE"