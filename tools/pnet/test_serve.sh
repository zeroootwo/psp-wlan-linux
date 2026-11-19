#!/bin/bash
# Host-side test of `pnet serve` + psp.py.  Everything runs in the foreground of this one command, the server is bound
# to 127.0.0.1 only, lives for at most 60 s, and is cleaned up on exit.  Needs gcc and python3.
set -u
cd "$(dirname "$0")"
W=$(mktemp -d); PORT=${PORT:-23230}
cleanup() { kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT
gcc -O1 -Wall -o "$W/pnet" pnet.c serve.c ntp.c wpa.c || exit 1
python3 -c "import secrets;print(secrets.token_hex(32))" > "$W/key"
cp "$W/key" "$W/wrongkey"; echo 00 >> /dev/null; python3 -c "import secrets;open('$W/wrongkey','w').write(secrets.token_hex(32))"
mkfifo "$W/in" "$W/out"
( while true; do sh < "$W/in" > "$W/out" 2>&1; done ) &
sleep 0.3
timeout 60 "$W/pnet" serve $PORT "$W/key" "$W/in" "$W/out" any 127.0.0.1 > "$W/serve.log" 2>&1 &
sleep 0.7
export PSP_HOST=127.0.0.1 PSP_KEY="$W/key"
sed "s/^PORT = 2323/PORT = $PORT/" psp.py > "$W/psp.py"
P="python3 $W/psp.py"
ok=1; t() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; ok=0; fi; }
t "ping"                    '[ "$($P ping)" = PONG ]'
t "run output"              '[ "$($P run "echo hello")" = hello ]'
t "run exit code"           '$P run "false"; [ $? -eq 1 ]'
t "run stderr merged"       '[ "$($P run "echo e 1>&2")" = e ]'
t "shell state kept"        '$P run "cd /; X=42" >/dev/null; [ "$($P run "pwd; echo \$X")" = "$(printf "/\n42")" ]'
head -c 300000 /dev/urandom > "$W/blob"
t "put 300 KB"              '$P put "$W/blob" "$W/blob.r" 755 >/dev/null && cmp -s "$W/blob" "$W/blob.r"'
t "put mode"                '[ "$(stat -c %a "$W/blob.r" 2>/dev/null || stat -f %Lp "$W/blob.r")" = 755 ]'
t "get 300 KB"              '$P get "$W/blob.r" "$W/blob.g" >/dev/null && cmp -s "$W/blob" "$W/blob.g"'
t "get missing file"        '! $P get "$W/nope" "$W/x" 2>/dev/null'
t "wrong key refused"       '! PSP_KEY="$W/wrongkey" $P ping 2>/dev/null'
t "interactive shell"       '[ "$(printf "echo via-shell\n" | timeout 8 $P shell | head -1)" = via-shell ]'
t "timeout reported"        '$P run -t 1 "sleep 3" 2>/dev/null; [ $? -eq 124 ]'
echo; [ $ok = 1 ] && echo "ALL OK" || echo "SOME TESTS FAILED"
[ $ok = 1 ]
