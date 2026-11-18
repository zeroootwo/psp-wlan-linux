# Troubleshooting (things that actually happened)

**The PSP hangs, the screen fills with noise, and it powers itself off later.**
A user-space program executed a floating point instruction. On this kernel the first FPU instruction freezes the whole machine. We hit it with
`fscanf("%lf")` in a helper. Keep floating point out of everything you run (`tools/pnet/build.sh` checks the binary).

**The PSP switches off right after the Wi-Fi LED lights up.**
Battery too weak for the chip's start-up current. Use the AC adapter.

**The PSP runs an old `auto.sh` (or the file turns up empty) after you copied a new one over USB.**
macOS keeps FAT writes in cache; leaving USB mode before it is flushed gives the PSP stale data. After copying: `fsync` each file and its
directory, `diskutil unmount` the volume, `diskutil mount` it again and compare the files, only then leave USB mode.

**`sleep: not found`, no `tail`, no `sync`, no `ifconfig`.**
The stock root filesystem is minimal. `pnet sleep N` replaces `sleep`; `pnet` replaces `ifconfig`, `ping`, a DHCP client and a tiny HTTP client.

**`link` works but the WPA handshake never starts (`msg1 seen 0`).**
The firmware still thinks it is associated: see "Reconnecting" in `PROTOCOL.md`. `link` tears the old association down first; if you drive the
commands by hand, send disassoc/deauth and clear the keys before associating again.

**The access point does not answer the association (about two seconds, then a short response with a non-zero status).**
Check the RSN IE: the pairwise suite must be CCMP (`00-0F-AC-04`), and the group suite must be the one the access point advertises.
The driver prints `LINK_FAIL ...` with a reason in `/proc/psp_wlan`.

**DHCP gets no answer although the link is up.**
Clear the BOOTP broadcast flag (the reply must be unicast), and use the chip's real MAC in `chaddr`.

**The PSP's own network settings do not list your network.**
Not our problem: the stock firmware hides networks it does not like (we saw it with a WPA2 home network that our driver found without trouble).
Use the manual SSID entry in the XMB if you need the stock stack.

**Not supported yet:** TKIP, WEP, WPA3, 5 GHz (the PSP has none), hidden SSIDs, power saving.
