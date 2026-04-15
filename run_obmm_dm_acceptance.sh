#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <ucx_dir>"
  exit 2
fi

UCX_DIR="$1"
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SMOKE_SRC="${SELF_DIR}/obmm_dm_smoke.c"
SMOKE_BIN="/tmp/obmm_dm_smoke"

if [[ -n "${LIBOBMM_DIR:-}" ]]; then
  export LD_LIBRARY_PATH="${LIBOBMM_DIR}:${LD_LIBRARY_PATH:-}"
fi

cd "${UCX_DIR}"

gcc -O2 -Wall -Wextra "${SMOKE_SRC}" \
  -I./src \
  -L./src/uct/.libs -L./src/ucs/.libs \
  -Wl,-rpath,"$PWD/src/uct/.libs" -Wl,-rpath,"$PWD/src/ucs/.libs" \
  -luct -lucs -ldl -lpthread \
  -o "${SMOKE_BIN}"

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
./src/tools/info/ucx_info -d | egrep -A2 'Transport: obmm|Device: obmm_sock'

echo "[5/5] obmm config listing"
./src/tools/info/ucx_info -c | grep '^OBMM_'

echo "ALL ACCEPTANCE STEPS PASSED"
