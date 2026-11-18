# wlanlog: trace what Sony's driver says to the WLAN chip

A kernel plugin for custom firmware (tested with PRO CFW on a PSP-3004, firmware 6.61). It hooks the block
read/write and register helpers inside Sony's `sceWlan_Driver` and logs every transfer with a timestamp to
`ms0:/wlanlog.bin` (status in `ms0:/wlanlog.txt`; it prints `PATCHED` when all hooks found their targets).

The hook offsets are those of the 6.61 PSP-3000 module; the plugin checks the first two instructions of each
target and refuses to patch if they do not match.

Build (needs the pspdev toolchain, e.g. the `pspdev/pspdev` Docker image):

    docker run --rm -v "$PWD":/src -w /src pspdev/pspdev make

Install: copy `wlanlog.prx` to `ms0:/seplugins/` and add `ms0:/seplugins/wlanlog.prx 1` to `ms0:/seplugins/VSH.TXT`.
Then connect to a network from the XMB and read `wlanlog.bin` over USB. Record format: see the comment at the top of
`main.c`. Trace of host buffers: writes appear 32-bit byte-swapped relative to the wire layout described in
`docs/PROTOCOL.md`.

Traces contain SSIDs, MAC addresses and (for WPA) session keys: do not publish them.
