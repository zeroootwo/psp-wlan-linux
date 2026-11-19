# The original, slow and safe driver settings (fallback when the fast ones fail).
echo "tune t_fw_us 0" > /proc/psp_wlan
echo "tune t_fw_wait_ms 20" > /proc/psp_wlan
echo "tune t_stop_ms 2" > /proc/psp_wlan
echo "tune t_int_us 700" > /proc/psp_wlan
echo "tune t_ack_ms 2" > /proc/psp_wlan
echo "tune t_resp_us 0" > /proc/psp_wlan
echo "tune t_poll_ms 10" > /proc/psp_wlan
echo "tune t_idle_ms 1" > /proc/psp_wlan
