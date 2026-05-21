#!/bin/bash
# run_osu_baseline.sh -- collect a focused obmm OSU performance baseline.
#
# Usage:
#   OSU_DIR=/path/to/osu HOSTFILE=hostfile ./run_osu_baseline.sh
# or:
#   OSU_DIR=/path/to/osu ./run_osu_baseline.sh node0 node1
#
# Optional env:
#   MPIRUN      - mpirun command (default: mpirun)
#   UCX_LOG     - UCX_LOG_LEVEL (default: warn)
#   UCX_TLS_SET - TLS list to force (default: obmm)
#   OBMM_MEMIDS - exported as UCX_OBMM_MEMIDS when set
#   OUT_DIR     - output directory (default: obmm_baseline_YYYYmmdd_HHMMSS)
#   ONLY        - run just one benchmark binary name, e.g. osu_alltoallv
#   NP_PT2PT    - np for 2-rank tests (default: 2)
#   NP_PAIRS    - np for multi-pair tests (default: 16)
#   NP_COLL     - np for collectives (default: 64)
#   SIZE_MIN    - min message size (default: 1)
#   SIZE_MAX    - max message size (default: 1048576)

set -uo pipefail

if [ -n "${1:-}" ] && [ -n "${2:-}" ]; then
    HOSTS="$1,$2"
fi
if [ -z "${HOSTS:-}" ] && [ -z "${HOSTFILE:-}" ]; then
    echo "usage: OSU_DIR=/path/to/osu HOSTFILE=hostfile $0"
    echo "   or: OSU_DIR=/path/to/osu $0 <node0_host> <node1_host>"
    exit 1
fi

if [ -z "${OSU_DIR:-}" ]; then
    echo "ERROR: set OSU_DIR to the OSU root directory"
    exit 1
fi

if [ ! -d "${OSU_DIR}/pt2pt" ] || [ ! -d "${OSU_DIR}/collective" ]; then
    echo "ERROR: expected ${OSU_DIR}/pt2pt and ${OSU_DIR}/collective"
    exit 1
fi

MPIRUN="${MPIRUN:-mpirun}"
UCX_LOG="${UCX_LOG:-warn}"
UCX_TLS_SET="${UCX_TLS_SET:-obmm}"
NP_PT2PT="${NP_PT2PT:-2}"
NP_PAIRS="${NP_PAIRS:-16}"
NP_COLL="${NP_COLL:-64}"
SIZE_MIN="${SIZE_MIN:-1}"
SIZE_MAX="${SIZE_MAX:-1048576}"
OUT_DIR="${OUT_DIR:-obmm_baseline_$(date +%Y%m%d_%H%M%S)}"
ONLY="${ONLY:-}"

mkdir -p "${OUT_DIR}"

if [ -n "${HOSTFILE:-}" ]; then
    HOST_OPT="--hostfile ${HOSTFILE}"
else
    HOST_OPT="-H ${HOSTS}"
fi

COMMON_OPTS="\
    ${HOST_OPT} \
    --map-by node \
    --mca pml ucx \
    --allow-run-as-root \
    -x OPAL_PREFIX \
    -x PATH \
    -x LD_LIBRARY_PATH \
    -x UCX_TLS=${UCX_TLS_SET} \
    -x UCX_OBMM_STATS=y \
    -x UCX_LOG_LEVEL=${UCX_LOG}"

if [ -n "${OBMM_MEMIDS:-}" ]; then
    COMMON_OPTS="${COMMON_OPTS} -x UCX_OBMM_MEMIDS=${OBMM_MEMIDS}"
fi

SIZE_OPT="-m ${SIZE_MIN}:${SIZE_MAX}"
overall_status=0

record_meta() {
    {
        echo "timestamp=$(date -Is)"
        echo "host_opt=${HOST_OPT}"
        echo "mpirun=${MPIRUN}"
        echo "ucx_tls=${UCX_TLS_SET}"
        echo "ucx_log=${UCX_LOG}"
        echo "obmm_memids=${OBMM_MEMIDS:-}"
        echo "np_pt2pt=${NP_PT2PT}"
        echo "np_pairs=${NP_PAIRS}"
        echo "np_coll=${NP_COLL}"
        echo "size_min=${SIZE_MIN}"
        echo "size_max=${SIZE_MAX}"
        echo "only=${ONLY}"
    } > "${OUT_DIR}/meta.env"

    if command -v ucx_info >/dev/null 2>&1; then
        ucx_info -d -t obmm > "${OUT_DIR}/ucx_info_obmm.txt" 2>&1 || true
        ucx_info -c | grep -i OBMM > "${OUT_DIR}/ucx_info_config.txt" 2>&1 || true
    fi
}

run_case() {
    local name="$1"
    local subdir="$2"
    local np="$3"
    local args="$4"
    local exe="${OSU_DIR}/${subdir}/${name}"
    local logfile="${OUT_DIR}/${name}.log"

    if [ -n "${ONLY}" ] && [ "${ONLY}" != "${name}" ]; then
        return 0
    fi
    if [ ! -x "${exe}" ]; then
        echo "SKIP ${name}: ${exe} not found" | tee -a "${OUT_DIR}/summary.log"
        return 0
    fi

    {
        echo "=================================================="
        echo "RUN ${name}"
        echo "CMD ${MPIRUN} -np ${np} ${COMMON_OPTS} ${exe} ${args}"
        echo "=================================================="
        ${MPIRUN} -np "${np}" ${COMMON_OPTS} "${exe}" ${args}
    } > "${logfile}" 2>&1 || overall_status=1

    tail -n 20 "${logfile}" | sed "s/^/[${name}] /" | tee -a "${OUT_DIR}/summary.log"
    echo >> "${OUT_DIR}/summary.log"
}

record_meta

run_case osu_latency   pt2pt      "${NP_PT2PT}" "${SIZE_OPT}"
run_case osu_bw        pt2pt      "${NP_PT2PT}" "${SIZE_OPT}"
run_case osu_bibw      pt2pt      "${NP_PT2PT}" "${SIZE_OPT}"
run_case osu_multi_lat pt2pt      "${NP_PAIRS}" "${SIZE_OPT}"
run_case osu_mbw_mr    pt2pt      "${NP_PAIRS}" "${SIZE_OPT}"
run_case osu_barrier   collective "${NP_COLL}"  ""
run_case osu_bcast     collective "${NP_COLL}"  "${SIZE_OPT}"
run_case osu_allreduce collective "${NP_COLL}"  "${SIZE_OPT}"
run_case osu_alltoall  collective "${NP_COLL}"  "${SIZE_OPT}"
run_case osu_alltoallv collective "${NP_COLL}"  "${SIZE_OPT}"

echo "Baseline logs written to ${OUT_DIR}"
echo "Suggested first inspection:"
echo "  grep -h \"obmm-stats\" ${OUT_DIR}/*.log"
echo "  grep -h \"^#\" ${OUT_DIR}/osu_*.log"
echo "  tail -n 50 ${OUT_DIR}/osu_alltoallv.log"

exit "${overall_status}"
