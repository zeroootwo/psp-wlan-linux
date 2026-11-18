#!/usr/bin/env python3
"""Host-side self test of the WPA2 primitives in wpa.c (SHA-1/HMAC/PBKDF2/PRF/AES unwrap/EAPOL frames).

    cc -O1 -o wpa_test wpa.c wpa_test.c && python3 selftest.py ./wpa_test

Reference values come from Python's hashlib/hmac, the IEEE 802.11i PMK vector, the RFC 3394 vector and
an RFC 3394 wrap built on top of `openssl enc -aes-128-ecb`.
"""
import hashlib, hmac, os, subprocess, sys

T = sys.argv[1] if len(sys.argv) > 1 else "./wpa_test"
ok = True


def run(*a):
    return subprocess.run([T, *a], capture_output=True, text=True).stdout


def chk(name, cond):
    global ok
    ok &= bool(cond)
    print(("PASS " if cond else "FAIL ") + name)


# PMK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 32)
vec = run("pmk", "password", "IEEE").strip().split("=")[1]
chk("802.11i PMK vector", vec == "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e")
for pw, ssid in (("ThisIsAPassword", "ThisIsASSID"), ("correct horse battery", "my-network")):
    exp = hashlib.pbkdf2_hmac("sha1", pw.encode(), ssid.encode(), 4096, 32).hex()
    chk("pmk %s" % ssid, run("pmk", pw, ssid).strip().split("=")[1] == exp)

for kl, dl in ((16, 50), (64, 100), (100, 3), (20, 200)):
    k, d = os.urandom(kl), os.urandom(dl)
    chk("hmac key %d data %d" % (kl, dl),
        run("hmac", k.hex(), d.hex()).strip().split("=")[1] == hmac.new(k, d, hashlib.sha1).hexdigest())


def prf(k, label, data, n):
    r, i = b"", 0
    while len(r) < n:
        r += hmac.new(k, label + b"\0" + data + bytes([i]), hashlib.sha1).digest()
        i += 1
    return r[:n]


pmk, aa, spa, an, sn = os.urandom(32), os.urandom(6), os.urandom(6), os.urandom(32), os.urandom(32)
data = min(aa, spa) + max(aa, spa) + min(an, sn) + max(an, sn)
got = run("ptk", pmk.hex(), aa.hex(), spa.hex(), an.hex(), sn.hex()).strip().split("=")[1]
chk("PTK (PRF-384)", got == prf(pmk, b"Pairwise key expansion", data, 48).hex())

r = run("unwrap", "000102030405060708090A0B0C0D0E0F", "1FA68B0A8112B447AEF34BD8FB5A7B829D3E862371D2CFE5")
chk("RFC 3394 vector", "rc=0" in r and "00112233445566778899aabbccddeeff" in r)


def ecb(kek, block):
    p = subprocess.run(["openssl", "enc", "-aes-128-ecb", "-nopad", "-K", kek.hex()], input=block, capture_output=True)
    assert p.returncode == 0 and len(p.stdout) == 16, p.stderr
    return p.stdout


def wrap(kek, pt):
    n = len(pt) // 8
    a, r = bytes.fromhex("A6A6A6A6A6A6A6A6"), [pt[8 * i:8 * i + 8] for i in range(n)]
    for j in range(6):
        for i in range(n):
            b = ecb(kek, a + r[i])
            a = bytes(x ^ y for x, y in zip(b[:8], (n * j + i + 1).to_bytes(8, "big")))
            r[i] = b[8:]
    return a + b"".join(r)


try:
    for n in (16, 24, 40, 56, 72):
        kek, pt = os.urandom(16), os.urandom(n)
        out = run("unwrap", kek.hex(), wrap(kek, pt).hex())
        chk("unwrap %d bytes" % n, "rc=0" in out and pt.hex() in out)
    out = run("unwrap", os.urandom(16).hex(), wrap(os.urandom(16), os.urandom(32)).hex())
    chk("wrong KEK rejected", "rc=-2" in out)
except Exception as e:  # openssl missing
    print("openssl-based unwrap tests skipped:", e)

kck, rp, snn = os.urandom(16), bytes.fromhex("0000000000000002"), os.urandom(32)
kd = bytes.fromhex("30140100000fac040100000fac040100000fac020000")
out = run("msg", "2", rp.hex(), snn.hex(), kck.hex(), kd.hex())
f = bytes.fromhex(out.split("frame=")[1].split()[0])
z = bytearray(f)
z[81:97] = b"\0" * 16
chk("msg2 MIC", hmac.new(kck, bytes(z), hashlib.sha1).digest()[:16] == f[81:97])
chk("msg2 layout", f[:2] == b"\x02\x03" and f[5:7] == b"\x01\x0a" and f[9:17] == rp and f[17:49] == snn and f[99:] == kd)
out4 = run("msg", "4", rp.hex(), snn.hex(), kck.hex(), "")
f4 = bytes.fromhex(out4.split("frame=")[1].split()[0])
chk("msg4 layout", f4[5:7] == b"\x03\x0a" and len(f4) == 99 and "micok=0" in out4)
print("ALL OK" if ok else "SOME TESTS FAILED")
sys.exit(0 if ok else 1)
