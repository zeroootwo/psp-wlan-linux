#!/usr/bin/env python3
"""psp.py: client for `pnet serve` (remote shell / commands / file transfer for the PSP running uClinux).

    psp.py ping
    psp.py run [-t SECONDS] COMMAND...        run one command line on the PSP, print output, exit with its status
    psp.py shell                              interactive (line based, no tty) session
    psp.py get REMOTE [LOCAL]
    psp.py put LOCAL REMOTE [MODE]            MODE octal, default 644 (use 755 for programs)
    psp.py find [SUBNET_PREFIX]               look for the PSP on the LAN, e.g. psp.py find 192.168.3

Host: $PSP_HOST, or ~/.psp-host (written by `find`).  Key: $PSP_KEY or ~/.psp-key (hex, same as /net/shell.key on the card).
Traffic is not encrypted (the login is a challenge-response, the session itself is plain): use it on a trusted network.
"""
import hashlib, hmac, os, select, socket, sys, concurrent.futures

PORT = 2323
HOSTFILE = os.path.expanduser("~/.psp-host")


def load_key():
    p = os.environ.get("PSP_KEY") or os.path.expanduser("~/.psp-key")
    try:
        return bytes.fromhex(open(p).read().strip())
    except OSError:
        sys.exit("no key: put the hex key from /net/shell.key into %s (or set PSP_KEY)" % p)


def host():
    h = os.environ.get("PSP_HOST")
    if not h and os.path.exists(HOSTFILE):
        h = open(HOSTFILE).read().strip()
    if not h:
        sys.exit("no host: set PSP_HOST or run `psp.py find <subnet prefix>`")
    return h


def recvn(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise ConnectionError("connection closed")
        b += c
    return b


def readline(s):
    b = b""
    while not b.endswith(b"\n"):
        c = s.recv(1)
        if not c:
            break
        b += c
    return b.rstrip(b"\r\n")


def connect(h=None, timeout=10):
    s = socket.create_connection((h or host(), PORT), timeout=timeout)
    hdr = recvn(s, 21)
    if hdr[:5] != b"PSP1 ":
        raise ConnectionError("not a pnet server: %r" % hdr[:8])
    s.sendall(hmac.new(load_key(), hdr[5:], hashlib.sha1).digest())
    r = readline(s)
    if r != b"OK":
        raise PermissionError("login refused (%s)" % r.decode(errors="replace"))
    return s


def cmd_ping(_a):
    s = connect()
    s.sendall(b"PING\n")
    print(readline(s).decode())


def cmd_run(a):
    t = 30
    if a[:1] == ["-t"]:
        t, a = int(a[1]), a[2:]
    s = connect(timeout=t + 15)
    s.sendall(("RUN %d %s\n" % (t, " ".join(a))).encode())
    h = readline(s).split()
    if not h or h[0] != b"OK":
        sys.exit("error: %s" % b" ".join(h).decode())
    n, code, trunc = int(h[1]), int(h[2]), int(h[3])
    sys.stdout.buffer.write(recvn(s, n))
    sys.stdout.flush()
    if trunc:
        print("[output truncated]", file=sys.stderr)
    if code == -2:
        print("[timed out; the command may still be running in the PSP shell]", file=sys.stderr)
        sys.exit(124)
    sys.exit(code)


def cmd_shell(_a):
    s = connect(timeout=None)
    s.sendall(b"SHELL\n")
    if readline(s) != b"OK":
        sys.exit("no shell on the PSP")
    s.settimeout(None)
    stdin = sys.stdin.buffer
    eof = False
    while True:
        # after the end of the input keep reading until the PSP has been quiet for 1.5 s (piped use: echo cmd | psp.py shell)
        r, _, _ = select.select([s] + ([] if eof else [stdin]), [], [], 1.5 if eof else None)
        if not r:
            return
        if s in r:
            d = s.recv(4096)
            if not d:
                return
            sys.stdout.buffer.write(d)
            sys.stdout.flush()
        if stdin in r:
            d = os.read(stdin.fileno(), 4096)
            if not d:
                eof = True
            else:
                s.sendall(d)


def cmd_get(a):
    rem, loc = a[0], (a[1] if len(a) > 1 else os.path.basename(a[0]))
    s = connect()
    s.sendall(("GET %s\n" % rem).encode())
    h = readline(s).split()
    if not h or h[0] != b"OK":
        sys.exit("error: %s" % b" ".join(h).decode())
    n = int(h[1])
    with open(loc, "wb") as f:
        left = n
        while left:
            c = s.recv(min(65536, left))
            if not c:
                sys.exit("connection lost")
            f.write(c)
            left -= len(c)
    print("%s: %d bytes" % (loc, n))


def cmd_put(a):
    loc, rem = a[0], a[1]
    mode = a[2] if len(a) > 2 else "644"
    data = open(loc, "rb").read()
    s = connect(timeout=60)
    s.sendall(("PUT %d %s %s\n" % (len(data), mode, rem)).encode())
    r = readline(s)
    if r != b"GO":
        sys.exit("error: %s" % r.decode())
    s.sendall(data)
    print(readline(s).decode())


def cmd_find(a):
    prefix = a[0] if a else ".".join(socket.gethostbyname(socket.gethostname()).split(".")[:3])

    def probe(i):
        ip = "%s.%d" % (prefix, i)
        try:
            c = socket.create_connection((ip, PORT), timeout=2.0)
            hdr = c.recv(5)
            c.close()
            return ip if hdr == b"PSP1 " else None
        except OSError:
            return None

    with concurrent.futures.ThreadPoolExecutor(32) as ex:
        found = [x for x in ex.map(probe, range(1, 255)) if x]
    if not found:
        sys.exit("no PSP found in %s.0/24" % prefix)
    open(HOSTFILE, "w").write(found[0] + "\n")
    print(found[0])


CMDS = {"ping": cmd_ping, "run": cmd_run, "shell": cmd_shell, "get": cmd_get, "put": cmd_put, "find": cmd_find}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in CMDS:
        sys.exit(__doc__)
    try:
        CMDS[sys.argv[1]](sys.argv[2:])
    except (OSError, ConnectionError, PermissionError) as e:
        sys.exit("psp.py: %s" % e)
