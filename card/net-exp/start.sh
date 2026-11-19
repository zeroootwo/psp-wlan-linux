# Wi-Fi bring-up for uClinux on PSP-3000.  Called from /ms0/auto.sh at boot.
# Needs on the card: /ms0/wlanfw/{fwh,fwm}.bin, /ms0/net/pnet, /ms0/wpa.conf (ssid=..., psk=... ; omit psk for an open network)
# wlan0 itself is registered by pnet connect after the first successful association.
# time zone: POSIX rule from /ms0/net/tz if present (default: Eastern Europe, EU summer time); the clock itself is UTC (set by NTP)
TZ="EET-2EEST,M3.5.0/3,M10.5.0/4"
if [ -f /ms0/net/tz ]; then
TZ=`cat /ms0/net/tz`
fi
export TZ
echo "export TZ=\"$TZ\"" >> /etc/profile
echo "== wifi start"
echo clr > /proc/psp_fwh
echo clr > /proc/psp_fwm
cat /ms0/wlanfw/fwh.bin > /proc/psp_fwh
cat /ms0/wlanfw/fwm.bin > /proc/psp_fwm
# bring the chip up: fast driver settings first, the slow safe ones if that fails
CHIP=0
for MODE in fast slow; do
if [ $CHIP = 0 ]; then
if [ $MODE = fast ]; then
if [ -f /ms0/net-exp/tune.sh ]; then
sh /ms0/net-exp/tune.sh
fi
else
sh /ms0/net-exp/tune-slow.sh
fi
echo go > /proc/psp_wlan
echo init > /proc/psp_wlan
echo fw > /proc/psp_wlan
cat /proc/psp_wlan > /fw.txt
if grep "FIRMWARE RUNNING" /fw.txt > /dev/null; then
echo chipinit > /proc/psp_wlan
cat /proc/psp_wlan > /ci.txt
if grep "CHIPINIT_OK" /ci.txt > /dev/null; then
CHIP=1
echo "== chip up with $MODE settings"
grep "FIRMWARE RUNNING" /fw.txt
fi
fi
if [ $CHIP = 0 ]; then
echo "== chip bring-up with $MODE settings FAILED"
cat /fw.txt
fi
fi
done
cp /ms0/net/pnet /bin/pnet
chmod 755 /bin/pnet
# remote access (shell, commands, file transfer): needs /ms0/net/shell.key, see tools/pnet/psp.py
if [ -f /ms0/net/shell.key ]; then
mknod /dev/psh_in p
mknod /dev/psh_out p
sh /ms0/net/shell-loop.sh &
pnet serve 2323 /ms0/net/shell.key /dev/psh_in /dev/psh_out wlan0 > /ms0/net/serve.log 2>&1 &
echo "== remote access listening on port 2323"
fi
if [ -f /ms0/net/nosup ]; then
echo "== supervisor NOT started (net/nosup present)"
else
echo "== supervisor started (log: /ms0/net/wifi.log)"
pnet connect wlan0 /ms0/wpa.conf > /ms0/net/wifi.log 2>&1 &
fi
# diagnostics dump (about 1 s of shell work) runs in the background so it does not delay the connection
sh /ms0/net/diag.sh > /ms0/diag.txt 2>&1 &
