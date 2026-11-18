# Runs at boot (the stock rc.sysinit starts /ms0/auto.sh in the background; its output goes to /ms0/log.txt)
sh /ms0/net/start.sh
if [ -f /ms0/net/test ]; then
sh /ms0/net/test.sh
fi
