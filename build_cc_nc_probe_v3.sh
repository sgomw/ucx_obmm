#!/usr/bin/env bash
set -eu
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
CC_BIN="${CC:-gcc}"
OUT="${1:-$SCRIPT_DIR/obmm_cc_nc_probe_v3}"
exec "$CC_BIN" -O3 -Wall -Wextra -std=gnu11 -o "$OUT" "$SCRIPT_DIR/cc_nc_probe_v3.c" -ldl
