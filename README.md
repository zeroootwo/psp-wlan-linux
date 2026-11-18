# pspwave: Wi-Fi for uClinux on the PSP-3000

*(repository: `psp-wlan-linux`)*

A working WLAN driver, a WPA2 supplicant and a connection supervisor for Linux (uClinux 2.6.22) on the **PSP-3000 / 3004**.
As far as we know it is the first time this Wi-Fi chip runs under Linux: the chip's host protocol was not documented
anywhere, so this repository also contains the protocol description ([docs/PROTOCOL.md](docs/PROTOCOL.md)) that we
worked out by watching Sony's own driver.

## Status

Verified on a PSP-3004 (PRO CFW; chip firmware taken from the 6.61 modules):

* firmware download and start of the WLAN chip, scanning, association to **open** and **WPA2-PSK (CCMP)** networks;
* a real `wlan0` network interface (DHCP, ICMP, DNS, TCP all work; round trip to the internet about 60-100 ms);
* a supervisor (`pnet connect`) that finds the network by name, joins it, runs the 4-way handshake and DHCP, watches the
  gateway with ARP, and reconnects by itself after a link loss. About 9 seconds from start to "connected".

Not done: TKIP, WEP, WPA3, hidden SSIDs, power saving, interrupt-driven operation (the driver polls every 10 ms, which is
plenty for internet access but not fast), and testing on other PSP models. Expect rough edges: this is a research
project that happens to work.

## How it works

```
 /ms0/net/pnet connect          link + WPA2 handshake (software) + DHCP + ARP watchdog       tools/pnet
        |  /proc/psp_wlan
 psp_wlan_probe.c               chip bring-up, firmware download, scan/join, wlan0 net_device  kernel/drivers/misc
        |  Memory-Stick-style transfers (MS2 host)
 WLAN chip (ARM, firmware loaded by the host)
```

Two things the chip firmware does *not* do: it has no WPA supplicant (the host runs the handshake and loads the keys with two
vendor commands), and it does not tell the access point about a re-association while it thinks it is still associated
(the driver tears the old association down first). Both are described in the protocol document.

## Repository layout

| path | what |
|------|------|
| `kernel/drivers/misc/psp_wlan_probe.c` | the driver (GPL-2.0) |
| `kernel/patches/` | patches against [Krush206/PSPLinux](https://github.com/Krush206/PSPLinux): build fixes, driver wiring |
| `kernel/config/psp-wlan.config` | kernel configuration we run |
| `tools/pnet/` | `pnet`: connect/DHCP/ping/HTTP/WPA2 helper for the PSP (no `fork`, no floating point); host-side crypto self test |
| `tools/fwextract/` | cut the chip firmware images out of your own PSP's firmware |
| `tools/wlanlog/` | CFW plugin that traces Sony's driver (how the protocol was found) |
| `card/` | the scripts and config that go on the memory stick |
| `docs/` | [PROTOCOL](docs/PROTOCOL.md), [BUILD](docs/BUILD.md), [INSTALL](docs/INSTALL.md), [TROUBLESHOOTING](docs/TROUBLESHOOTING.md) |

## Quick start

1. Build the kernel and `pnet`: [docs/BUILD.md](docs/BUILD.md).
2. Get the chip firmware out of your own PSP: [tools/fwextract](tools/fwextract/extract_wlanfw.py).
3. Put everything on the memory stick: [docs/INSTALL.md](docs/INSTALL.md).
4. Start "uClinux" from the XMB. After a minute the PSP is on your network.

## Warnings from experience

* **Never run floating point code** on this kernel: the first FPU instruction hangs the machine.
* Use the AC adapter while experimenting: the chip's start-up current can shut a weak battery down.
* After copying files from a Mac, flush the write cache and remount before leaving USB mode, or the PSP will run stale files.

Details in [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).

## Legal

This project contains **no Sony code and no Sony firmware**. The chip firmware images must be extracted from your own PSP.
The protocol description is based on observing how the chip behaves, for the purpose of interoperability. "PlayStation" and "PSP"
are trademarks of Sony Interactive Entertainment; this project is not affiliated with or endorsed by Sony. Use it at your own
risk: nothing here writes to the PSP's flash, but running custom firmware and homebrew is your responsibility.

## Credits

* Anastasiia Velikaya: driver, tools, protocol research.
* The uClinux-on-PSP port: Jackson Mo (original port), the *pspux* project (toolchain, loader), and
  [Krush206/PSPLinux](https://github.com/Krush206/PSPLinux) (the kernel tree these patches apply to).
* [pspdecrypt](https://github.com/John-K/pspdecrypt) and the PSP homebrew community for the tooling around firmware modules.

## License

GPL-2.0, see `COPYING`. The patches in `kernel/patches/` are derivative of the upstream GPL-2.0 kernel sources.
