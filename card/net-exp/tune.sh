# Fast driver settings tried first at every boot (the slow defaults in tune-slow.sh are the fallback).
# Each line: tune <name> <value>.  Values can also be changed at run time: echo "tune t_stop_ms 0" > /proc/psp_wlan
echo "tune t_fw_us 100" > /proc/psp_wlan
echo "tune t_fw_wait_ms 1" > /proc/psp_wlan
echo "tune t_stop_ms 0" > /proc/psp_wlan
echo "tune t_int_us 20" > /proc/psp_wlan
echo "tune t_ack_ms 0" > /proc/psp_wlan
echo "tune t_resp_us 200" > /proc/psp_wlan
echo "tune t_poll_ms 4" > /proc/psp_wlan
echo "tune t_idle_ms 0" > /proc/psp_wlan
