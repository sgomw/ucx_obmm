#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <ucx_prefix> <output_dir>"
  exit 2
fi

UCX_PREFIX="$1"
OUT_DIR="$2"
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SMOKE_SRC="${SELF_DIR}/obmm_dm_smoke.c"
RUN_SCRIPT="${SELF_DIR}/run_obmm_dm_acceptance.sh"

if [[ ! -f "${SMOKE_SRC}" ]]; then
  echo "missing source: ${SMOKE_SRC}"
  exit 3
fi

if [[ ! -f "${RUN_SCRIPT}" ]]; then
  echo "missing run script: ${RUN_SCRIPT}"
  exit 4
fi

mkdir -p "${OUT_DIR}"

LIB_FLAGS=()
if [[ -d "${UCX_PREFIX}/lib" ]]; then
  LIB_FLAGS+=("-L${UCX_PREFIX}/lib")
fi
if [[ -d "${UCX_PREFIX}/lib64" ]]; then
  LIB_FLAGS+=("-L${UCX_PREFIX}/lib64")
fi

if [[ ${#LIB_FLAGS[@]} -eq 0 ]]; then
  echo "no UCX lib dir found under ${UCX_PREFIX} (expect lib or lib64)"
  exit 5
fi

gcc -O2 -Wall -Wextra "${SMOKE_SRC}" \
  -I"${UCX_PREFIX}/include" \
  "${LIB_FLAGS[@]}" \
  -luct -lucs -ldl -lpthread \
  -o "${OUT_DIR}/obmm_dm_smoke"

cp -f "${RUN_SCRIPT}" "${OUT_DIR}/run_obmm_dm_acceptance.sh"

echo "bundle prepared in ${OUT_DIR}"
echo "files:"
echo "  ${OUT_DIR}/obmm_dm_smoke"
echo "  ${OUT_DIR}/run_obmm_dm_acceptance.sh"
