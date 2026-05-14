#!/bin/bash
# run_v3_ucx_perftest.sh -- staged UCT-level obmm AM tests.
#
# Run one copy on the server node and one copy on the client node:
#
#   server:
#     UCX_OBMM_MEM_MODE=hybrid UCX_OBMM_NC_MEMIDS=1,2 \
#     UCX_OBMM_CC_MEMIDS=3,4 ./run_v3_ucx_perftest.sh server
#
#   client:
#     UCX_OBMM_MEM_MODE=hybrid UCX_OBMM_NC_MEMIDS=1,2 \
#     UCX_OBMM_CC_MEMIDS=3,4 ./run_v3_ucx_perftest.sh client <server-host>
#
# Important: UCT perftest must use -d memory -x obmm. UCX_TLS is a UCP-level
# setting and does not select the UCT transport for these tests.
#
# Optional env:
#   UCX_PERFTEST  - ucx_perftest binary path (default: ucx_perftest)
#   UCX_LOG       - UCX_LOG_LEVEL value (default: warn)
#   PERF_STAGE    - short | bcopy | smoke | all (default: all)
#   BASE_PORT     - first TCP control port (default: 13337)
#   SHORT_SIZES   - space-separated short sizes
#   BCOPY_SIZES   - space-separated bcopy sizes; mode-aware default
#   NITERS_LAT    - latency iterations (default: 10000)
#   NITERS_BW     - bandwidth iterations (default: 100000)
#   SKIP_INFO     - set to 1 to skip ucx_info capability print

set -e

ROLE="$1"
PEER="$2"

if [ "$ROLE" != "server" ] && [ "$ROLE" != "client" ]; then
    echo "usage: $0 server"
    echo "   or: $0 client <server-host>"
    exit 1
fi
if [ "$ROLE" = "client" ] && [ -z "$PEER" ]; then
    echo "ERROR: client role requires <server-host>"
    exit 1
fi

UCX_PERFTEST="${UCX_PERFTEST:-ucx_perftest}"
UCX_OBMM_MEM_MODE="${UCX_OBMM_MEM_MODE:-nc}"
UCX_LOG_LEVEL="${UCX_LOG_LEVEL:-${UCX_LOG:-warn}}"
PERF_STAGE="${PERF_STAGE:-all}"
BASE_PORT="${BASE_PORT:-13337}"
NITERS_LAT="${NITERS_LAT:-10000}"
NITERS_BW="${NITERS_BW:-100000}"
SHORT_SIZES="${SHORT_SIZES:-8 64 1024 2000}"

if [ -z "$UCX_OBMM_NC_MEMIDS" ]; then
    echo "ERROR: UCX_OBMM_NC_MEMIDS is mandatory for obmm v3"
    exit 1
fi

case "$UCX_OBMM_MEM_MODE" in
    nc)
        if [ -n "$UCX_OBMM_CC_MEMIDS" ]; then
            echo "WARN: UCX_OBMM_CC_MEMIDS is set but ignored in nc mode"
        fi
        BCOPY_SIZES="${BCOPY_SIZES:-1 1024 4096}"
        ;;
    hybrid)
        if [ -z "$UCX_OBMM_CC_MEMIDS" ]; then
            echo "ERROR: UCX_OBMM_CC_MEMIDS is mandatory in hybrid mode"
            exit 1
        fi
        BCOPY_SIZES="${BCOPY_SIZES:-1 4096 8192 16384}"
        ;;
    *)
        echo "ERROR: UCX_OBMM_MEM_MODE must be nc or hybrid, got '$UCX_OBMM_MEM_MODE'"
        exit 1
        ;;
esac

export UCX_OBMM_MEM_MODE UCX_OBMM_NC_MEMIDS UCX_OBMM_CC_MEMIDS UCX_LOG_LEVEL

if ! command -v "$UCX_PERFTEST" >/dev/null 2>&1; then
    echo "ERROR: $UCX_PERFTEST not found"
    exit 1
fi

if [ "${SKIP_INFO:-0}" != "1" ] && command -v ucx_info >/dev/null 2>&1; then
    echo "=================================================="
    echo "ucx_info obmm capabilities on this node"
    echo "=================================================="
    ucx_info -d -u t -t obmm || true
    echo
fi

case_index=0

stage_enabled() {
    local stage="$1"
    [ "$PERF_STAGE" = "all" ] || [ "$PERF_STAGE" = "$stage" ] || \
        { [ "$PERF_STAGE" = "smoke" ] && [ "$stage" = "short" ]; }
}

run_case() {
    local name="$1"
    local test="$2"
    local layout="$3"
    local size="$4"
    local iters="$5"
    local port=$((BASE_PORT + case_index))
    case_index=$((case_index + 1))

    echo "=================================================="
    echo "RUN ${name}: role=${ROLE} mode=${UCX_OBMM_MEM_MODE} port=${port} size=${size} iters=${iters}"
    echo "=================================================="

    if [ "$ROLE" = "server" ]; then
        "$UCX_PERFTEST" -d memory -x obmm -t "$test" -D "$layout" -p "$port"
    else
        "$UCX_PERFTEST" "$PEER" -d memory -x obmm -t "$test" -D "$layout" \
            -p "$port" -s "$size" -n "$iters"
    fi
    echo
}

if stage_enabled short; then
    for size in $SHORT_SIZES; do
        run_case "am_lat_short_${size}" "am_lat" "short" "$size" "$NITERS_LAT"
    done
    if [ "$PERF_STAGE" != "smoke" ]; then
        for size in $SHORT_SIZES; do
            run_case "am_bw_short_${size}" "am_bw" "short" "$size" "$NITERS_BW"
        done
    fi
fi

if stage_enabled bcopy && [ "$PERF_STAGE" != "smoke" ]; then
    for size in $BCOPY_SIZES; do
        run_case "am_lat_bcopy_${size}" "am_lat" "bcopy" "$size" "$NITERS_LAT"
    done
    for size in $BCOPY_SIZES; do
        run_case "am_bw_bcopy_${size}" "am_bw" "bcopy" "$size" "$NITERS_BW"
    done
fi

echo "ALL UCT PERFTEST CASES DONE"
