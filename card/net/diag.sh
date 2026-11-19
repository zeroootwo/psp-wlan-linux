# One-shot hardware/kernel diagnostics, written to /ms0/diag.txt at every boot (read it over USB if remote access is down).
echo "== date";    date
echo "== cp0";     echo cp0 > /proc/psp_dbg; cat /proc/psp_dbg
echo "== mem";     echo mem > /proc/psp_dbg; cat /proc/psp_dbg
echo "== free";    free
echo "== meminfo"; cat /proc/meminfo
echo "== cpuinfo"; cat /proc/cpuinfo
echo "== interrupts"; cat /proc/interrupts
echo "== ps";      ps
echo "== mounts";  cat /proc/mounts
echo "== devices"; cat /proc/devices
echo "== done"
