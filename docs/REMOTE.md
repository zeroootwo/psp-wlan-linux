# Remote access over Wi-Fi

`pnet serve` (tools/pnet/serve.c) gives you a shell, one-shot commands and file transfer on the PSP without touching the
memory stick. It listens on TCP port 2323 and only talks to hosts in the PSP's own subnet. The login is a
challenge-response (HMAC-SHA1 of a random nonce with a shared key); the session itself is **not encrypted**, use it on a
network you trust.

## Setup

1. Make a key: `python3 -c 'import os;print(os.urandom(20).hex())' > ~/.psp-key`
2. Put the same line on the card as `/net/shell.key` (hex). `card/net/start.sh` starts the server when that file exists.
3. On the Mac/PC: `tools/pnet/psp.py find 192.168.1` (finds the PSP and remembers its address in `~/.psp-host`).

## Use

```
psp.py ping
psp.py run -t 30 'cat /proc/uptime'      # one command, prints output, exits with its status
psp.py shell                             # line-based interactive session (no tty)
psp.py put local.bin /ms0/some/file 755  # upload (written to .tmp first, then renamed)
psp.py get /ms0/log.txt log.txt
```

`poweroff` powers the console down (the kernel stops feeding the power controller's watchdog; the screen may show
"Stop pacifying watchdog" for a few seconds). There is no remote restart.

Writes go to a FAT memory stick: they are flushed by the server, but pull the card only after `poweroff`.

## Time

`pnet ntp` sets the clock from the network at boot; the time zone is a POSIX rule from `/net/tz` (default in start.sh).
