# Wi-Fi bring-up for uClinux on PSP-3000.  Called from /ms0/auto.sh at boot.
# Needs on the card: /ms0/wlanfw/{fwh,fwm}.bin, /ms0/net/pnet, /ms0/wpa.conf (ssid=..., psk=... ; omit psk for an open network)
# wlan0 itself is registered by `pnet connect` after the first successful association.
echo "== wifi start"
echo go > /proc/psp_wlan
echo init > /proc/psp_wlan
echo clr > /proc/psp_fwh
echo clr > /proc/psp_fwm
cat /ms0/wlanfw/fwh.bin > /proc/psp_fwh
cat /ms0/wlanfw/fwm.bin > /proc/psp_fwm
echo fw > /proc/psp_wlan
echo chipinit > /proc/psp_wlan
cat /proc/psp_wlan
cp /ms0/net/pnet /bin/pnet
chmod 755 /bin/pnet
if [ -f /ms0/net/nosup ]; then
echo "== supervisor NOT started (net/nosup present)"
else
echo "== supervisor started (log: /ms0/net/wifi.log)"
pnet connect wlan0 /ms0/wpa.conf > /ms0/net/wifi.log 2>&1 &
fi
