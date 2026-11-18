# Installing on the PSP

Tested on a PSP-3004 running PRO custom firmware (chip firmware taken from the 6.61 modules) with a Memory Stick PRO Duo. It needs a custom firmware that can run
homebrew in kernel mode (the stock uClinux loader is a CFW homebrew). Nothing is written to the PSP's flash.

## Memory stick layout

```
/PSP/GAME/uClinux_wlan/EBOOT.PBP      loader       (from uclinux_on_psp-0.22.zip)
/PSP/GAME/uClinux_wlan/kmodlib.prx    loader       (same)
/PSP/GAME/uClinux_wlan/pspboot.conf   kernel=vmlinux-wlan.bin   cmdline=console=tty osk=Dv4
/PSP/GAME/uClinux_wlan/vmlinux-wlan.bin   the kernel you built (docs/BUILD.md)
/auto.sh                              from card/auto.sh
/net/start.sh  /net/test.sh           from card/net/
/net/pnet                             built from tools/pnet
/wlanfw/fwh.bin  /wlanfw/fwm.bin      chip firmware images (tools/fwextract)
/wpa.conf                             copy of card/wpa.conf.example, edited
```

`wpa.conf` is plain text: `ssid=...` and `psk=...` (the passphrase, 8-63 characters). Omit the `psk` line for an open network.
The file contains your Wi-Fi password: keep it off the internet.

## Run

Start "uClinux" from the XMB game menu. About 45 seconds later (the chip firmware download alone takes ~20 s)
the PSP is online. `net/wifi.log` on the card shows what the supervisor did:

```
[   49] link ok: bssid=.. ch=11 secure=1 ...
wpa: msg3 OK (MIC verified), sent msg4
dhcp: ACK ip 192.168.1.50 mask 255.255.255.0 gw 192.168.1.1 ...
[   51] CONNECTED
```

`pnet connect` keeps running: it probes the gateway with ARP every 5 seconds and reconnects from scratch after three misses,
and renews the DHCP lease. The log is rewritten at every boot.

Useful commands in the on-screen console: `pnet ping 8.8.8.8`, `pnet get example.com /` (plain HTTP, prints the first
3 KB), `pnet sleep N`, `cat /proc/net/dev`, `dmesg`.

## When something goes wrong

* Make a test run: create the empty files `/net/test` and `/net/nosup` on the card. The PSP then runs `net/test.sh`, which
  prints a marker for every stage into `/log.txt`, saves `dmesg` into `/dmesg.txt`, simulates a link loss, and powers off at the end.
* `docs/TROUBLESHOOTING.md` lists the problems we have seen.
* Use the AC adapter while testing: the WLAN chip's start-up current can shut a weak battery down.
