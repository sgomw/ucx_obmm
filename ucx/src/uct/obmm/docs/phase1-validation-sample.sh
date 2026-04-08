#!/usr/bin/env bash
set -euo pipefail

# Phase-1 validation sample:
# 1) Build UCX with the new obmm UCT skeleton
# 2) Verify ucx_info can enumerate the new transport

./autogen.sh
./contrib/configure-release --enable-debug
make -j"$(nproc)"

./src/tools/info/ucx_info -d | grep -iE '(^#.*Transport|obmm)'
