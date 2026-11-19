#!/bin/bash
# Download throughput of the PSP from a temporary HTTP server on this Mac (serves one random file, dies by itself after 120 s).
# usage: bench_http.sh [size_bytes]   (needs psp.py working and PSP_HOST/~/.psp-host set)
SIZE=${1:-1000000}
HERE="$(cd "$(dirname "$0")" && pwd)"
IP=$(ipconfig getifaddr en0 2>/dev/null || ifconfig | awk '/inet 192\.168/{print $2; exit}')
D=$(mktemp -d); head -c "$SIZE" /dev/urandom > "$D/blob.bin"
cat > "$D/srv.py" <<PY
import http.server, socketserver, signal, sys, os
signal.alarm(120); os.chdir(sys.argv[1]); socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(("$IP", 8099), http.server.SimpleHTTPRequestHandler) as s: s.serve_forever()
PY
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$D"' EXIT
python3 "$D/srv.py" "$D" >/dev/null 2>&1 &
sleep 1.2
OUT=$(python3 "$HERE/psp.py" run -t 100 'S=`cat /proc/uptime`; pnet get '"$IP"' /blob.bin 8099 | sed -n "/bytes total/p"; E=`cat /proc/uptime`; echo "$S"; echo "$E"' 2>&1)
echo "$OUT" | python3 -c "
import sys,re
t=sys.stdin.read().split('\n'); n=[l for l in t if 'bytes total' in l]; ups=[l.split()[0] for l in t if re.match(r'^[0-9]+\.[0-9]+ ',l)]
if n and len(ups)==2:
    b=int(re.search(r'(\d+) bytes total',n[0]).group(1)); dt=float(ups[1])-float(ups[0]); print('%d bytes in %.2f s = %.1f KB/s = %.2f Mbit/s' % (b,dt,b/dt/1000,b*8/dt/1e6))
else: print('benchmark failed:', ' | '.join(t[-4:]))"
