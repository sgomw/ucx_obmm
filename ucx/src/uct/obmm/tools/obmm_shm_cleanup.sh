#!/bin/bash
#
# Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
#
# See file LICENSE for terms.
#
# obmm_shm_cleanup.sh — convenience wrapper for obmm_shm_cleanup
#
# Usage:
#   ./obmm_shm_cleanup.sh <memid> <size>
#
#   memid  — decimal memid (e.g. 1 → /dev/obmm_shmdev1)
#   size   — region size, decimal or hex (e.g. 268435456 or 0x10000000)
#
# Examples:
#   ./obmm_shm_cleanup.sh 1 0x10000000    # zero /dev/obmm_shmdev1, 256 MiB
#   ./obmm_shm_cleanup.sh 2 268435456     # zero /dev/obmm_shmdev2, 256 MiB

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY="${SCRIPT_DIR}/obmm_shm_cleanup"

if [ $# -ne 2 ]; then
    echo "Usage: $(basename "$0") <memid> <size>"
    echo "  memid  — decimal memid (e.g. 1 → /dev/obmm_shmdev1)"
    echo "  size   — region size, decimal or hex (e.g. 0x10000000 for 256 MiB)"
    exit 1
fi

# Auto-compile if the binary is missing or the source is newer.
if [ ! -x "${BINARY}" ] || [ "${SCRIPT_DIR}/obmm_shm_cleanup.c" -nt "${BINARY}" ]; then
    echo "obmm_shm_cleanup.sh: compiling ${BINARY} ..."
    make -C "${SCRIPT_DIR}" -j
fi

exec "${BINARY}" "$@"
