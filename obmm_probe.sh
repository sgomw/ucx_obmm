#!/usr/bin/env bash
# OBMM 设备访问探测 —— 定位 ucx_info -d 的 EPERM 来源。
# 用法：sudo ./obmm_probe.sh    或    ./obmm_probe.sh
# 不需要编译，仅依赖 bash + coreutils + dd。

set -u

DEV=${1:-/dev/obmm_shmdev2}

echo "==== 1. 当前用户 ===="
id

echo
echo "==== 2. 设备节点权限 ===="
ls -l /dev/obmm_shmdev* 2>/dev/null || echo "no /dev/obmm_shmdev* present"

echo
echo "==== 3. open(RDWR) 不带 O_SYNC ===="
# bash 9<> 重定向 = open(O_RDWR)，不会带 O_SYNC
if (exec 9<>"$DEV") 2>/tmp/obmm_open_rdwr.err; then
    echo "OK: open($DEV, O_RDWR) succeeded"
else
    echo "FAIL: open($DEV, O_RDWR) failed:"
    cat /tmp/obmm_open_rdwr.err
fi

echo
echo "==== 4. open(RDWR | O_SYNC) ===="
# dd iflag=sync 会在 open 时带 O_SYNC；count=0 不实际读，只是验证 open
if dd if="$DEV" iflag=sync of=/dev/null bs=1 count=0 status=none \
        2>/tmp/obmm_open_sync.err; then
    echo "OK: open($DEV, O_RDWR | O_SYNC) succeeded"
else
    echo "FAIL: open($DEV, O_RDWR | O_SYNC) failed:"
    cat /tmp/obmm_open_sync.err
fi

echo
echo "==== 5. open(RDONLY) ===="
if (exec 9<"$DEV") 2>/tmp/obmm_open_rd.err; then
    echo "OK: open($DEV, O_RDONLY) succeeded"
else
    echo "FAIL: open($DEV, O_RDONLY) failed:"
    cat /tmp/obmm_open_rd.err
fi

echo
echo "==== 6. sysfs 关键属性（用于交叉印证）===="
SYSDIR=/sys/devices/obmm
if [ -d "$SYSDIR" ]; then
    for d in "$SYSDIR"/obmm_shmdev*; do
        [ -d "$d" ] || continue
        echo "--- $d ---"
        for k in type size allow_mmap; do
            if [ -r "$d/$k" ]; then
                printf "  %-12s = %s\n" "$k" "$(cat "$d/$k" 2>/dev/null)"
            fi
        done
    done
else
    echo "no $SYSDIR"
fi

echo
echo "==== 诊断指引 ===="
cat <<'EOF'
- 3 OK / 4 FAIL  → 驱动拒绝 O_SYNC，需要改 obmm_region.c 拿 NC 映射的方式
- 3 FAIL / 5 OK  → 设备节点对当前用户只读，加用户到 owner group / udev rule
- 3 FAIL / 5 FAIL → 纯权限/capability，sudo 再跑一次本脚本对照
- 全 OK          → 可能是 ucx_info 路径里别的设备触发，看 6 段哪个 dev 对应 EPERM
EOF
