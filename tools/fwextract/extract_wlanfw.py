#!/usr/bin/env python3
"""Cut the two WLAN chip firmware images out of Sony's wlanfirm_03g module.

The images are Sony's property and are NOT distributed with this project: you extract them from
your own PSP's firmware.  Steps:

  1. Get wlanfirm_03g.prx from your PSP (dump flash0 from a custom firmware, or unpack the official
     6.61 update package).  The file is encrypted ("~PSP").
  2. Decrypt it with pspdecrypt (https://github.com/John-K/pspdecrypt):  pspdecrypt wlanfirm_03g.prx
  3. python3 extract_wlanfw.py wlanfirm_03g.prx.dec   ->  fwh.bin (helper) and fwm.bin (main)
  4. Copy both to the memory stick: /wlanfw/fwh.bin and /wlanfw/fwm.bin

Input may be the decrypted PRX (ELF) or just its first loadable segment.  Offsets are those of the
03g module from firmware 6.61 (PSP-3000); the script checks the SHA-1 of the results against the
values we extracted and tells you if your build differs.
"""
import hashlib
import struct
import sys

# offsets inside the first loadable segment
HELPER = (0x84, 2140)
MAIN = (0x8E0, 95228)
KNOWN = {
    "fwh.bin": "150ef7fda656c238202fbbfe0f3507778953b794",
    "fwm.bin": "b94cb025d36f5d599388db25ba67129b8bc5b8b0",
}


def segment(data):
    if data[:4] == b"\x7fELF":
        phoff = struct.unpack_from("<I", data, 0x1C)[0]
        n = struct.unpack_from("<H", data, 0x2C)[0]
        for i in range(n):
            ptype, off, _va, _pa, filesz = struct.unpack_from("<5I", data, phoff + 32 * i)
            if ptype == 1:
                return data[off:off + filesz]
        sys.exit("no PT_LOAD segment in the ELF")
    if data[:4] == b"~PSP":
        sys.exit("this file is still encrypted: run pspdecrypt on it first")
    return data


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    seg = segment(open(sys.argv[1], "rb").read())
    for name, (off, ln) in (("fwh.bin", HELPER), ("fwm.bin", MAIN)):
        blob = seg[off:off + ln]
        if len(blob) != ln:
            sys.exit("%s: module too short, wrong file?" % name)
        open(name, "wb").write(blob)
        sha = hashlib.sha1(blob).hexdigest()
        print("%s  %d bytes  sha1 %s  %s" % (name, ln, sha,
              "(matches the known good image)" if sha == KNOWN[name] else
              "WARNING: differs from the image we used, the offsets may be different for your firmware"))


main()
