# The persistent shell behind `pnet serve`: stdin/stdout are the two FIFOs the server holds open.
# Restarted automatically if somebody types `exit`.
while true; do
sh < /dev/psh_in > /dev/psh_out 2>&1
done
