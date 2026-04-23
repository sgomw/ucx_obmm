 ./obmm_probe.sh /dev/obmm_shmdev2 2>&1 | tail -40
 echo "----"
 ls /sys/devices/obmm/
 echo "----"
 for d in /sys/devices/obmm/obmm_shmdev*; do
   echo "=== $d ==="
   cat "$d/type" "$d/size" "$d/allow_mmap" 2>/dev/null
 done
 echo "----"
 sudo ./obmm_probe.sh /dev/obmm_shmdev2 2>&1 | grep -E 'open|FAIL|OK'
