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
OBMM_REQUIRED_SYMS=(obmm_export_useraddr obmm_unexport obmm_import obmm_unimport obmm_preimport obmm_unpreimport)

check_libobmm_runtime() {
  local candidates=()
  local c p sym missing=0

  echo "[0/5] libobmm precheck"
  echo "LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-<EMPTY>}"

  if [[ -n "${LIBOBMM_DIR:-}" ]]; then
    echo "LIBOBMM_DIR=${LIBOBMM_DIR}"
    for c in "${LIBOBMM_DIR}"/libobmm.so*; do
      [[ -f "${c}" ]] && candidates+=("${c}")
    done
  fi

  if command -v ldconfig >/dev/null 2>&1; then
    while IFS= read -r p; do
      [[ -n "${p}" && -f "${p}" ]] && candidates+=("${p}")
    done < <(ldconfig -p 2>/dev/null | grep -E 'libobmm\.so' | awk '{print $NF}')
  fi

  if [[ ${#candidates[@]} -eq 0 ]]; then
    echo "FAIL: no libobmm shared library found via LIBOBMM_DIR/ldconfig"
    return 1
  fi

  echo "libobmm candidates:"
  for c in "${candidates[@]}"; do
    echo "  ${c}"
  done

  if command -v nm >/dev/null 2>&1; then
    echo "symbol check (nm -D):"
    for c in "${candidates[@]}"; do
      echo "  checking ${c}"
      missing=0
      for sym in "${OBMM_REQUIRED_SYMS[@]}"; do
        if ! nm -D "${c}" 2>/dev/null | grep -wq "${sym}"; then
          echo "    missing: ${sym}"
          missing=1
        fi
      done
      if [[ ${missing} -eq 0 ]]; then
        echo "    PASS: required symbols present"
        return 0
      fi
    done
    echo "FAIL: libobmm found, but required symbols are incomplete"
    return 1
  fi

  if command -v readelf >/dev/null 2>&1; then
    echo "symbol check (readelf -Ws):"
    for c in "${candidates[@]}"; do
      echo "  checking ${c}"
      missing=0
      for sym in "${OBMM_REQUIRED_SYMS[@]}"; do
        if ! readelf -Ws "${c}" 2>/dev/null | grep -wq "${sym}"; then
          echo "    missing: ${sym}"
          missing=1
        fi
      done
      if [[ ${missing} -eq 0 ]]; then
        echo "    PASS: required symbols present"
        return 0
      fi
    done
    echo "FAIL: libobmm found, but required symbols are incomplete"
    return 1
  fi

  echo "WARN: nm/readelf unavailable, skip symbol check"
  return 0
}

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

if ! check_libobmm_runtime; then
  exit 7
fi

echo "[1/5] normal lifecycle test"
"${SMOKE_BIN}"

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
