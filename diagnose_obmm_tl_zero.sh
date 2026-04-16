#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <ucx_prefix>"
  exit 2
fi

UCX_PREFIX="$1"
UCX_INFO="${UCX_PREFIX}/bin/ucx_info"
CTL_GLOB="${UCX_OBMM_CTL_GLOB:-/sys/devices/ub_bus_controller*/*/ubc}"

if [[ ! -x "${UCX_INFO}" ]]; then
  echo "ucx_info not found: ${UCX_INFO}"
  exit 3
fi

for lib_dir in "${UCX_PREFIX}/lib" "${UCX_PREFIX}/lib64"; do
  if [[ -d "${lib_dir}" ]]; then
    export LD_LIBRARY_PATH="${lib_dir}:${LD_LIBRARY_PATH:-}"
  fi
done

if [[ -n "${LIBOBMM_DIR:-}" ]]; then
  export LD_LIBRARY_PATH="${LIBOBMM_DIR}:${LD_LIBRARY_PATH:-}"
fi

echo "=== obmm tl zero-device diagnostic ==="
echo "UCX_PREFIX=${UCX_PREFIX}"
echo "UCX_OBMM_CTL_GLOB=${CTL_GLOB}"
echo

echo "--- ucx_info -c | OBMM_ ---"
if ! "${UCX_INFO}" -c | grep '^OBMM_'; then
  echo "(no OBMM_ config found)"
fi
echo

echo "--- ucx_info -d | obmm ---"
if ! "${UCX_INFO}" -d | grep -E -A2 'Transport: obmm|Device: obmm_sock'; then
  echo "(no obmm transport/device listed)"
fi
echo

echo "--- controller glob match and attributes ---"
shopt -s nullglob
paths=( ${CTL_GLOB} )
echo "matched_paths=${#paths[@]}"
if [[ ${#paths[@]} -eq 0 ]]; then
  echo "reason_hint: ctl_glob did not match any path"
  exit 0
fi

ok_cnt=0
for p in "${paths[@]}"; do
  if [[ -d "${p}" ]]; then
    base="${p}"
  else
    base="${p%/*}"
  fi

  echo "== match=${p} =="
  echo "   attr_base=${base}"
  if [[ -r "${p}" && ! -d "${p}" ]]; then
    ubc_val="$(cat "${p}" 2>/dev/null || true)"
    echo "   ubc_value=${ubc_val:-<EMPTY>}"
  fi

  miss=0
  for f in eid numa primary_cna ummu_map; do
    if [[ -r "${base}/${f}" ]]; then
      v="$(cat "${base}/${f}" 2>/dev/null || true)"
      if [[ -z "${v}" ]]; then
        echo "  ${f}=<EMPTY>"
        miss=1
      else
        echo "  ${f}=${v}"
      fi
    else
      echo "  ${f}=<MISS/NO_READ>"
      miss=1
    fi
  done
  if [[ ${miss} -eq 0 ]]; then
    ok_cnt=$((ok_cnt + 1))
  fi
done

echo
echo "controllers_with_all_required_attrs=${ok_cnt}"
if [[ ${ok_cnt} -eq 0 ]]; then
  echo "reason_hint: all matched controllers were filtered by missing/invalid attrs"
else
  echo "reason_hint: controllers look readable; if TL is still 0, focus on NUMA/socket mapping and runtime permissions"
fi
