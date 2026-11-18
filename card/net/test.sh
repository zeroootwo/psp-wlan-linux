# Staged test, runs only when /ms0/net/test exists (create net/nosup too if you want this script to start the supervisor).
# Every stage prints a marker and saves dmesg to the card, so a hang shows where it happened.
# (the stock rootfs has no sleep/tail/sync: `pnet sleep N` is used instead)
echo "== STAGE 1: chip initialised, idle 15 s"
dmesg > /ms0/dmesg.txt
pnet sleep 15
echo "== STAGE 2: one ping with no interface"
pnet ping 8.8.8.8 1
dmesg > /ms0/dmesg.txt
echo "== STAGE 3: starting the supervisor"
pnet connect wlan0 /ms0/wpa.conf > /ms0/net/wifi.log 2>&1 &
pnet sleep 45
echo "== wifi.log after 45 s"
cat /ms0/net/wifi.log
dmesg > /ms0/dmesg.txt
echo "== STAGE 4: ping"
pnet ping 8.8.8.8 3
echo "== STAGE 5: simulated link loss (the supervisor must notice and reconnect by itself)"
echo > /ms0/net/simloss
pnet sleep 80
echo "== wifi.log after simulated link loss"
cat /ms0/net/wifi.log
dmesg > /ms0/dmesg.txt
echo "== STAGE 6: ping + http"
pnet ping 8.8.8.8 3
pnet get example.com /
cat /proc/net/dev
dmesg > /ms0/dmesg.txt
echo "== DONE"
reboot
