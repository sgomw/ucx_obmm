#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <ucx_prefix> [smoke_bin]"
  exit 2
fi

UCX_PREFIX="$1"
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SMOKE_BIN="${2:-${SELF_DIR}/obmm_dm_smoke}"
UCX_INFO="${UCX_PREFIX}/bin/ucx_info"
DIAG_SCRIPT="${SELF_DIR}/diagnose_obmm_tl_zero.sh"

if [[ ! -x "${SMOKE_BIN}" ]]; then
  echo "smoke binary not found or not executable: ${SMOKE_BIN}"
  exit 3
fi

if [[ ! -x "${UCX_INFO}" ]]; then
  echo "ucx_info not found: ${UCX_INFO}"
  exit 4
fi

for lib_dir in "${UCX_PREFIX}/lib" "${UCX_PREFIX}/lib64"; do
  if [[ -d "${lib_dir}" ]]; then
    export LD_LIBRARY_PATH="${lib_dir}:${LD_LIBRARY_PATH:-}"
  fi
done

if [[ -n "${LIBOBMM_DIR:-}" ]]; then
  export LD_LIBRARY_PATH="${LIBOBMM_DIR}:${LD_LIBRARY_PATH:-}"
fi

echo "[1/5] normal lifecycle test"
if ! "${SMOKE_BIN}"; then
  echo "normal lifecycle test failed"
  if [[ -x "${DIAG_SCRIPT}" ]]; then
    echo
    echo "[diag] running obmm zero-device diagnostic"
    "${DIAG_SCRIPT}" "${UCX_PREFIX}" || true
  fi
  exit 7
fi

echo "[2/5] quota limit test"
UCX_OBMM_GRANULARITY=4k \
UCX_OBMM_MAX_EXPORT_BYTES=4k \
UCX_OBMM_MAX_EXPORTS=1 \
"${SMOKE_BIN}" --quota-limit

echo "[3/5] no-controller path test"
UCX_OBMM_CTL_GLOB='/no/such/ub_bus_controller*/*/ubc' \
"${SMOKE_BIN}" --expect-no-tl

echo "[4/5] transport/device listing"
if ! "${UCX_INFO}" -d | grep -E -A2 'Transport: obmm|Device: obmm_sock'; then
  echo "no obmm transport/device listed by ucx_info -d"
  exit 5
fi

echo "[5/5] obmm config listing"
if ! "${UCX_INFO}" -c | grep '^OBMM_'; then
  echo "no OBMM_ configs listed by ucx_info -c"
  exit 6
fi

echo "ALL ACCEPTANCE STEPS PASSED"
