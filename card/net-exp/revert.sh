# Safety net for live driver tuning: sh /ms0/net-exp/revert.sh <seconds> &
# After <seconds> the slow safe settings are restored unless the file /keep exists.
pnet sleep $1
if [ ! -f /keep ]; then
sh /ms0/net-exp/tune-slow.sh
fi
