#!/usr/bin/env bash
# OBMM device access probe.
# Usage: ./obmm_probe.sh [/dev/obmm_shmdevN]
# No compilation required. Tries multiple methods to open with O_SYNC.

set -u

DEV=${1:-/dev/obmm_shmdev2}

echo "==== 1. user ===="
id

echo
echo "==== 2. device node permissions ===="
ls -l /dev/obmm_shmdev* 2>/dev/null || echo "no /dev/obmm_shmdev* present"

echo
echo "==== 3. open(RDWR) without O_SYNC ===="
if (exec 9<>"$DEV") 2>/tmp/obmm_open_rdwr.err; then
    echo "OK: open($DEV, O_RDWR) succeeded"
else
    echo "FAIL: open($DEV, O_RDWR) failed:"
    cat /tmp/obmm_open_rdwr.err
fi

echo
echo "==== 4. open(RDWR|O_SYNC) -- multiple methods ===="

method4_done=0

# Method 4a: dd iflag=sync (fails on some coreutils variants)
if [ "$method4_done" = 0 ]; then
    err=$(dd if="$DEV" iflag=sync of=/dev/null bs=1 count=0 status=none 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "OK (via dd iflag=sync): open with O_SYNC succeeded"
        method4_done=1
    elif echo "$err" | grep -qi 'invalid'; then
        echo "skip dd iflag=sync (not supported by this dd)"
    else
        echo "FAIL (via dd iflag=sync): $err"
        method4_done=1
    fi
fi

# Method 4b: dd oflag=sync (write side)
if [ "$method4_done" = 0 ]; then
    err=$(dd if=/dev/zero of="$DEV" oflag=sync bs=1 count=0 status=none 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "OK (via dd oflag=sync): open with O_SYNC succeeded"
        method4_done=1
    elif echo "$err" | grep -qi 'invalid'; then
        echo "skip dd oflag=sync (not supported)"
    else
        echo "FAIL (via dd oflag=sync): $err"
        method4_done=1
    fi
fi

# Method 4c: perl Fcntl
if [ "$method4_done" = 0 ] && command -v perl >/dev/null 2>&1; then
    err=$(perl -e 'use Fcntl qw(O_RDWR O_SYNC);
                   sysopen(my $f, $ARGV[0], O_RDWR|O_SYNC) or die "$!\n";
                   close($f); print "perl_open_ok\n";' "$DEV" 2>&1)
    if echo "$err" | grep -q perl_open_ok; then
        echo "OK (via perl): open with O_SYNC succeeded"
        method4_done=1
    else
        echo "FAIL (via perl): $err"
        method4_done=1
    fi
fi

if [ "$method4_done" = 0 ]; then
    echo "skip: no working O_SYNC open method available on this host"
    echo "      (need either coreutils dd with iflag=sync or perl)"
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
echo "==== 6. sysfs key attrs ===="
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
