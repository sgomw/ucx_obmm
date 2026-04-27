#!/bin/bash
# run_osu_tests.sh -- run the OSU Micro-Benchmarks over the obmm transport.
#
# Usage:
#     OSU_DIR=/path/to/osu ./run_osu_tests.sh node0 node1
# or:
#     OSU_DIR=...; HOSTFILE=hostfile ./run_osu_tests.sh
#
# OSU_DIR must contain the category subdirs directly:
#     $OSU_DIR/pt2pt/osu_latency
#     $OSU_DIR/collective/osu_allreduce
#     $OSU_DIR/startup/osu_init
#     ...
#
# Optional env:
#     MPIRUN     - mpirun command (default: mpirun)
#     UCX_LOG    - UCX_LOG_LEVEL (default: warn)
#     EXTRA_OPTS - extra mpirun args
#     CAT        - run only one category: pt2pt | coll | nbcoll | startup
#                  (default: all runnable categories)
#     NP_PT2PT   - np for 2-rank pt2pt tests (default: 2; some OSU tests
#                  insist on exactly 2)
#     NP_PAIRS   - np for multi-pair pt2pt (osu_multi_lat, osu_mbw_mr).
#                  MUST be even. Default: 16
#     NP_COLL    - np for collectives. Default: 32. Cap is 256 ranks per
#                  node (UCT_OBMM_POOL_SLOT_COUNT) -> 512 across 2 nodes;
#                  but very large NPs make some OSU collectives take
#                  minutes per size. Start small.
#     SIZE_MIN   - -m min message size for size-sweep tests. Default: 1
#     SIZE_MAX   - -m max for size-sweep tests. Default: 1048576 (1 MiB)
#                  (OSU default goes to 4 MiB; we cap at 1 MiB so total
#                  runtime is bounded. Bump if you want larger.)
#
# WHAT DOES NOT RUN AND WHY -- see end of script for the full list of
# OSU benchmarks we deliberately skip.
#
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

if [ -z "$OSU_DIR" ]; then
    echo "ERROR: set OSU_DIR to your OSU root (the dir containing"
    echo "       pt2pt/  collective/  startup/  ...)"
    exit 1
fi

OSU_BIN="${OSU_DIR}"
if [ ! -d "${OSU_BIN}/pt2pt" ] || [ ! -d "${OSU_BIN}/collective" ]; then
    echo "ERROR: expected ${OSU_BIN}/pt2pt and ${OSU_BIN}/collective"
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
NP_PT2PT="${NP_PT2PT:-2}"
NP_PAIRS="${NP_PAIRS:-16}"
NP_COLL="${NP_COLL:-32}"
SIZE_MIN="${SIZE_MIN:-1}"
SIZE_MAX="${SIZE_MAX:-1048576}"
CAT="${CAT:-all}"

# obmm-only TLS. Self stays for self-loopback wireup.
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

run_osu() {
    local category="$1"   # pt2pt|coll|nbcoll|startup -- gating tag
    local subdir="$2"     # subdir under $OSU_BIN
    local bin="$3"        # binary name (e.g. osu_latency)
    local np="$4"         # ranks
    local args="$5"       # benchmark-specific args

    if [ "$CAT" != "all" ] && [ "$CAT" != "$category" ]; then
        return 0
    fi
    local exe="${OSU_BIN}/${subdir}/${bin}"
    if [ ! -x "$exe" ]; then
        echo "SKIP ${bin}: ${exe} not found"
        return 0
    fi

    echo "=================================================="
    echo "RUN  ${bin}  np=${np}  args='${args}'"
    echo "=================================================="
    ${MPIRUN} -np ${np} ${COMMON_OPTS} ${exe} ${args} || {
        echo "FAIL ${bin} (continuing)"
    }
    echo
}

SIZE_OPT="-m ${SIZE_MIN}:${SIZE_MAX}"

#-------------------------------------------------------------------
# 1. POINT-TO-POINT  (blocking)
#    np = 2 except multi-pair tests
#-------------------------------------------------------------------
run_osu pt2pt pt2pt osu_latency       "${NP_PT2PT}" "${SIZE_OPT}"
run_osu pt2pt pt2pt osu_bw            "${NP_PT2PT}" "${SIZE_OPT}"
run_osu pt2pt pt2pt osu_bibw          "${NP_PT2PT}" "${SIZE_OPT}"
# multi-pair latency: NP_PAIRS ranks form NP_PAIRS/2 pairs across nodes
run_osu pt2pt pt2pt osu_multi_lat     "${NP_PAIRS}" "${SIZE_OPT}"
# multi-bandwidth, msg-rate: same shape
run_osu pt2pt pt2pt osu_mbw_mr        "${NP_PAIRS}" "${SIZE_OPT}"
# message rate (newer OSU). harmless if absent.
run_osu pt2pt pt2pt osu_latency_mt    "${NP_PT2PT}" "${SIZE_OPT}"
run_osu pt2pt pt2pt osu_latency_mp    "${NP_PT2PT}" "${SIZE_OPT}"

#-------------------------------------------------------------------
# 2. COLLECTIVES  (blocking)
#    np scales -- start with NP_COLL (default 32). For very large
#    sizes use -m to cap; collective+large-size+many-ranks blows up
#    runtime fast.
#-------------------------------------------------------------------
run_osu coll collective osu_barrier    "${NP_COLL}" ""
run_osu coll collective osu_bcast      "${NP_COLL}" "${SIZE_OPT}"
run_osu coll collective osu_reduce     "${NP_COLL}" "${SIZE_OPT}"
run_osu coll collective osu_allreduce  "${NP_COLL}" "${SIZE_OPT}"
run_osu coll collective osu_gather     "${NP_COLL}" "${SIZE_OPT}"
run_osu coll collective osu_gatherv    "${NP_COLL}" "${SIZE_OPT}"
run_osu coll collective osu_allgather  "${NP_COLL}" "${SIZE_OPT}"
run_osu coll collective osu_allgatherv "${NP_COLL}" "${SIZE_OPT}"
run_osu coll collective osu_scatter    "${NP_COLL}" "${SIZE_OPT}"
run_osu coll collective osu_scatterv   "${NP_COLL}" "${SIZE_OPT}"
# alltoall(v) is N^2 traffic -- be careful with large NP_COLL.
run_osu coll collective osu_alltoall   "${NP_COLL}" "${SIZE_OPT}"
run_osu coll collective osu_alltoallv  "${NP_COLL}" "${SIZE_OPT}"
run_osu coll collective osu_reduce_scatter "${NP_COLL}" "${SIZE_OPT}"

#-------------------------------------------------------------------
# 3. NON-BLOCKING COLLECTIVES (overlap-style; needs MPI_THREAD progress
#    or OMPI's request-based progress -- works over am)
#-------------------------------------------------------------------
run_osu nbcoll collective osu_iallreduce  "${NP_COLL}" "${SIZE_OPT}"
run_osu nbcoll collective osu_ibcast      "${NP_COLL}" "${SIZE_OPT}"
run_osu nbcoll collective osu_ialltoall   "${NP_COLL}" "${SIZE_OPT}"
run_osu nbcoll collective osu_ireduce     "${NP_COLL}" "${SIZE_OPT}"
run_osu nbcoll collective osu_iallgather  "${NP_COLL}" "${SIZE_OPT}"
run_osu nbcoll collective osu_iallgatherv "${NP_COLL}" "${SIZE_OPT}"
run_osu nbcoll collective osu_igather     "${NP_COLL}" "${SIZE_OPT}"
run_osu nbcoll collective osu_igatherv    "${NP_COLL}" "${SIZE_OPT}"
run_osu nbcoll collective osu_iscatter    "${NP_COLL}" "${SIZE_OPT}"
run_osu nbcoll collective osu_iscatterv   "${NP_COLL}" "${SIZE_OPT}"
run_osu nbcoll collective osu_ialltoallv  "${NP_COLL}" "${SIZE_OPT}"
run_osu nbcoll collective osu_ibarrier    "${NP_COLL}" ""

#-------------------------------------------------------------------
# 4. STARTUP / MISC
#-------------------------------------------------------------------
run_osu startup startup osu_init   "${NP_COLL}" ""
run_osu startup startup osu_hello  "${NP_COLL}" ""

echo "=================================================="
echo "ALL OSU RUNNABLE TESTS DONE"
echo "=================================================="
echo
cat <<'EOF'
=========================================================
Tests we INTENTIONALLY DO NOT RUN (and why):
=========================================================

A) MPI one-sided / RMA (osu/one-sided/*):
     osu_get_acc_latency, osu_get_bw, osu_get_latency,
     osu_put_bibw, osu_put_bw, osu_put_latency,
     osu_acc_latency, osu_fop_latency, osu_cas_latency
   WHY: These exercise MPI_Put / MPI_Get / MPI_Accumulate / etc,
   which UCX serves via uct_ep_put_*, uct_ep_get_*, uct_atomic_*.
   The obmm transport currently advertises NONE of those caps --
   only am_short + am_bcopy. With UCX_TLS=obmm,self, OMPI's UCX
   OSC component (osc/ucx) cannot find a transport with the
   required RMA caps and either errors at MPI_Win_create or
   silently falls through to osc/pt2pt (which emulates RMA on
   send/recv). Either way the result is not a meaningful test of
   obmm RMA -- so we skip them entirely.
   To exercise them: implement put/get/atomic in obmm OR allow
   UCX_TLS to include a real RMA transport like rc/tcp.

B) GPU / device-buffer variants (osu_*_d_d, osu_*_h_d, *_cuda, *_rocm,
   *_neuron, etc):
   WHY: no GPU in this test environment, and obmm has no
   memory-type registration plumbing (it doesn't implement
   uct_md_mem_type_query / uct_iface_mem_type_*). Even with a GPU
   present, allocation kind would be 'host' only.

C) UPC / OpenSHMEM / Java / Python OSU variants:
   WHY: out of scope -- we only validate the MPI binding.

D) osu_persistent_*  (if your OSU build has them):
   WHY: should work over am (they reduce to send/recv), but they
   stress different code paths than blocking pt2pt and we already
   cover those. Add them if you want extra soak.

E) Multi-threaded:
   osu_latency_mt, osu_latency_mp -- INCLUDED above. They DO run
   over am, but obmm currently has no multi-thread test in our
   own suite, so treat the result as informational. If you see
   crashes or wrong answers with -t > 1, that's a worker/iface
   threading bug to triage separately.

F) Anything > NP_COLL ranks per node beyond 256:
   WHY: hard limit UCT_OBMM_POOL_SLOT_COUNT = 256 (compile-time).
   Per-job hard cap = 256 * node_count. With 2 nodes that is 512.
   Going above will fail at iface_open with
       "obmm: failed to allocate FIFO slot"
EOF
