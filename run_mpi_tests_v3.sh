#!/bin/bash
# run_mpi_tests_v3.sh -- staged MPI/UCP tests for obmm v3.
#
# Usage:
#     UCX_OBMM_NC_MEMIDS=1,2 UCX_OBMM_CC_MEMIDS=3,4 \
#     UCX_OBMM_MEM_MODE=hybrid ./run_mpi_tests_v3.sh node0 node1
#
# or:
#     HOSTFILE=hostfile UCX_OBMM_MEM_MODE=hybrid \
#     UCX_OBMM_NC_MEMIDS=1,2 UCX_OBMM_CC_MEMIDS=3,4 ./run_mpi_tests_v3.sh
#
# Optional env:
#     MODE            - nc | hybrid | both. Defaults to UCX_OBMM_MEM_MODE or hybrid.
#     STAGE           - smoke | boundary | pressure | multi | all (default: all)
#     MPIRUN          - mpirun command (default: mpirun)
#     UCX_LOG         - UCX_LOG_LEVEL (default: warn)
#     EXTRA_OPTS      - extra mpirun args
#     ONLY            - single test label: sanity | correctness_smoke |
#                       correctness_boundary | pressure | multi
#     MULTI_NP        - np for mpi_multi_v3 (default: 8; try 16, 32, 40)
#     CORRECT_MAX     - max size for boundary correctness (default: 1048576)
#     PRESSURE_SIZE   - message size for pressure test (default: 32768)
#     PRESSURE_WINDOW - in-flight messages per rank (default: 64)

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
MODE="${MODE:-${UCX_OBMM_MEM_MODE:-hybrid}}"
STAGE="${STAGE:-all}"
MULTI_NP="${MULTI_NP:-8}"
CORRECT_MAX="${CORRECT_MAX:-1048576}"
PRESSURE_SIZE="${PRESSURE_SIZE:-32768}"
PRESSURE_WINDOW="${PRESSURE_WINDOW:-64}"

if [ -z "$UCX_OBMM_NC_MEMIDS" ]; then
    echo "ERROR: UCX_OBMM_NC_MEMIDS is mandatory for obmm v3"
    exit 1
fi

case "$MODE" in
    nc|hybrid|both) ;;
    *)
        echo "ERROR: MODE must be nc, hybrid, or both, got '$MODE'"
        exit 1
        ;;
esac

if [ "$MODE" = "hybrid" ] || [ "$MODE" = "both" ]; then
    if [ -z "$UCX_OBMM_CC_MEMIDS" ]; then
        echo "ERROR: UCX_OBMM_CC_MEMIDS is mandatory for hybrid tests"
        exit 1
    fi
fi

COMMON_BASE="\
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

stage_enabled() {
    local stage="$1"
    [ "$STAGE" = "all" ] || [ "$STAGE" = "$stage" ]
}

only_enabled() {
    local label="$1"
    [ -z "$ONLY" ] || [ "$ONLY" = "$label" ]
}

run_mpi() {
    local mode="$1"
    local label="$2"
    local np="$3"
    local bin="$4"
    local extra_env="$5"
    local ucx_env="-x UCX_OBMM_MEM_MODE=${mode} -x UCX_OBMM_NC_MEMIDS=${UCX_OBMM_NC_MEMIDS}"

    if ! only_enabled "$label"; then
        return 0
    fi
    if [ ! -x "$bin" ]; then
        echo "SKIP ${label}: ${bin} not found / not executable"
        return 0
    fi
    if [ "$mode" = "hybrid" ]; then
        ucx_env="${ucx_env} -x UCX_OBMM_CC_MEMIDS=${UCX_OBMM_CC_MEMIDS}"
    fi

    echo "=================================================="
    echo "RUN ${label} mode=${mode} np=${np}"
    echo "=================================================="
    ${MPIRUN} -np ${np} ${COMMON_BASE} ${ucx_env} ${extra_env} ${bin}
    echo
}

run_suite() {
    local mode="$1"

    echo "##################################################"
    echo "OBMM V3 MPI SUITE mode=${mode}"
    echo "##################################################"

    if stage_enabled smoke; then
        run_mpi "$mode" sanity 2 ./mpi_sanity ""
        run_mpi "$mode" correctness_smoke 2 ./mpi_correctness_v3 \
            "-x OBMM_TEST_ITERS=20 -x OBMM_TEST_WINDOW=4 -x OBMM_TEST_MAX_SIZE=4096"
    fi

    if stage_enabled boundary; then
        run_mpi "$mode" correctness_boundary 2 ./mpi_correctness_v3 \
            "-x OBMM_TEST_ITERS=100 -x OBMM_TEST_WINDOW=8 -x OBMM_TEST_MAX_SIZE=${CORRECT_MAX}"
    fi

    if stage_enabled pressure; then
        run_mpi "$mode" pressure 2 ./mpi_pressure_v3 \
            "-x OBMM_PRESSURE_ITERS=200 -x OBMM_PRESSURE_WINDOW=${PRESSURE_WINDOW} -x OBMM_PRESSURE_SIZE=${PRESSURE_SIZE}"
    fi

    if stage_enabled multi; then
        run_mpi "$mode" multi "${MULTI_NP}" ./mpi_multi_v3 \
            "-x OBMM_TEST_ITERS=50 -x OBMM_TEST_MAX_SIZE=65536"
    fi
}

case "$MODE" in
    both)
        run_suite nc
        run_suite hybrid
        ;;
    nc|hybrid)
        run_suite "$MODE"
        ;;
esac

echo "ALL V3 MPI TESTS DONE"
