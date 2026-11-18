# PSP-3000 WLAN chip: host protocol as observed

This document describes how a host talks to the WLAN chip of a PSP-3000/3004 (an ARM-based Wi-Fi chip whose firmware is loaded by the host at every boot; its
strings and command style point to the Marvell 88W8380 family, but we have not verified the exact part). It was written from observation:
we traced the stock Sony driver talking to the chip (see `tools/wlanlog`), read how it behaves, and verified every
statement here on real hardware with our own driver (`kernel/drivers/misc/psp_wlan_probe.c`). Where the meaning of a
field is not known, it says so. Values were verified on a PSP-3004 with the chip image taken from the 6.61 firmware.

Nothing here is guaranteed for other PSP models or chip firmware versions.

## 1. Layers

```
802.11 / network      commands + data frames          (sections 5-8)
chip register file    8/16/32-bit registers, 2 ports   (sections 3-4)
host controller       Memory-Stick-style TPC transfers (section 2)
```

## 2. Host controller

The chip hangs off the second Memory Stick host controller ("MS2") at `0xBD300000`. The register layout is that of
the ordinary Memory Stick host: `CMD +0x30`, `DATA +0x34`, `STATUS +0x38`, `SYS +0x3C`, plus `+0x40` (DMA enable/latch
bits). The chip is driven with Memory-Stick-Pro style TPC transfers: *get interrupt status*, *set register address*,
*read/write register*, *read/write page data*, and *extended set command*. The driver (`ms_*` functions) implements
them with polling; the interrupt line (IRQ 8) is not needed.

Power-up: enable the MS2 clocks/IO through SYSREG, then assert the chip reset through SYSCON ("reset device 4",
held about 100 ms) and release it. Details are in the driver (`power_on`/`run_probe`).

## 3. Chip register file

Registers below are addressed with *set register address* followed by *read/write register*. 16-bit values are
big-endian on the wire.

| reg | size | meaning |
|-----|------|---------|
| 0x22 | 8 | signature, reads 0x24 when the chip is alive |
| 0x40 | 16 | length of a received data frame waiting to be read (0 = none) |
| 0x42 | 16 | length of the data frame about to be written (multiple of 4) |
| 0x44 | 16 | length of the command block (or firmware chunk) about to be written |
| 0x4A | 16 / 32 | 16-bit: length of the pending command response. 32-bit: reads `0x46554755` once the firmware runs |
| 0x4C | 16 | firmware download: chunk length the chip wants next (bit 0 = retry) |
| 0x4E | 32 | code of an asynchronous event |
| 0x54 | 8 | status (read): bit0 idle/TX done, bit1 data frame received, bit2 chip ready for the next block, bit3 async event, bit4 command response ready |
| 0x56 | 8 | acknowledge (write): 0x10 after reading a command response, 0x08 / 0x04 during firmware download |
| 0x5A 0x5C 0x5E | 8 | event masks; the stock driver writes 0x7F to each right after the firmware starts |

Writing `0xFB` to reg 0x54 after sending a command block tells the chip the block is complete. Writing `0xF7` to
reg 0x54 acknowledges an asynchronous event (bit 3).

## 4. Block transfers and firmware download

Data moves in *blocks* to or from two chip addresses (ports): **0x100** carries host commands and their responses,
**0x102** carries 802.11 data frames. A block transfer is started with *extended set command* and a 7-byte
payload `{cmd, len_hi, len_lo, addr[4]}` where `cmd` is `0xB4` (write), `0xB2` or `0xB3` (read). The data then flows in pages of
up to 0x200 bytes, and the transfer is closed with another *extended set command* `0x25` (stop). Lengths are rounded
up to a multiple of 4. 32-bit words of blocks written by the host are byte-swapped relative to the buffer the host
code builds (a trace of the stock driver shows the swapped form; reads are not swapped).

**Firmware download** (two stages, chip-driven):

1. Check reg 0x22 == 0x24.
2. Stage 1 ("helper", 2140 bytes for the 3000-series image): for each chunk of up to 0x600 bytes, set reg 0x44 to
   the length, write the block to port 0x100; then reg 0x44 = 0, reg 0x54 = 0xFB, reg 0x56 = 8.
3. Stage 2 (main image, 95228 bytes): the chip dictates the chunk length in reg 0x4C; send that many bytes to port
   0x100, acknowledge, repeat until done.
4. Poll reg 0x4A (32-bit) for `0x46554755`: the firmware is running (about 20 s in total, most of it PIO speed).
5. Write 0x7F to regs 0x5E, 0x5C, 0x5A.

The images belong to Sony and are not part of this project; see `tools/fwextract`.

## 5. Host commands

Commands travel through port 0x100. A command block is

```
u16 cmd   u16 size   u16 seq   u16 result(0)   payload...     (little endian; padded to a multiple of 4)
```

`size` is the number of meaningful bytes including the 8-byte header (it is not always the padded length). To send:
set reg 0x44 to the padded length, write the block to port 0x100, write 0xFB to reg 0x54, wait for reg 0x54 bit 4,
read the response length from reg 0x4A, read that many bytes (rounded up to 4) from port 0x100, write 0x10 to reg
0x56. The response has `cmd | 0x8000`, the same `seq`, and `result == 0` on success.

Commands used by the driver (payload offsets are from the start of the block, i.e. include the 8-byte header):

| cmd | size | meaning / layout |
|-----|------|------------------|
| 0x03 | 0x2E | hardware spec; the response carries the MAC address at offset 16 |
| 0x06 | 0x48 | scan: `[10]` BSS type (1 infrastructure, 3 any), `[11..16]` BSSID (ff.. = any), `[17..48]` SSID (32 bytes, zero padded), `[52..64]` channel list 1..13, `[66]` = 6, `[68]` = 0x64 |
| 0x11 | 0x11 | authenticate (open system): `[8..13]` BSSID |
| 0x12 | 0x18 | associate: `[8..13]` BSSID, `[16..17]` capability to send (0x21 = ESS + short preamble, add 0x10 for privacy), `[18..19]` listen interval (3) |
| 0x24 | 0x10 | deauthenticate: `[8..13]` BSSID, `[14..15]` reason |
| 0x26 | 0x10 | disassociate: same layout |
| 0x2F | 0x0C | privacy on/off: payload `01 00 01 00` on, `01 00 00 00` off |
| 0x30 | 0x10B | RSN information element for the association request: `[8..9] = 01 00`, then the IE exactly as it must appear on the air (`30 14 ...`) |
| 0x34 | 0x2A | key slot A: `u16 action` then a 32-byte key (key first, zero padded) |
| 0x35 | 0x2A | key slot B: same layout |
| 0x10 | 0xCC | multicast filter list (observed in the stock driver's flow; not needed for unicast operation) |
| 0x1F | 0x10 | polled by the stock driver every 200 ms; looks like link quality, contents not decoded |
| 0x1C 0x1E 0x20 0x22 0x16 0x28 0x7A 0x7D | various | configuration values replayed exactly as the stock driver sends them (see `init_cmds[]` in the driver); their purpose is not decoded |

Key slot actions (confirmed on hardware for CCMP): **cmd 0x34 action 3 = pairwise key (the 16-byte TK)**,
**cmd 0x35 action 5 = group key (16-byte GTK)**. The matching "clear" actions are the even numbers: cmd 0x34 actions 0 and 2,
cmd 0x35 actions 0, 2, 4, 6. Other actions presumably select other ciphers (TKIP, WEP) and are untested.

### Scan response (`0x8006`)

Byte `0x20` is the number of BSS entries, entries start at `0x21`: `u16 length`, then
`BSSID[6]`, timestamp `[8]`, beacon interval `u16`, capability `u16`, followed by the information elements as received
(SSID, rates, DS channel, RSN, ...).

### Association response (`0x8012`)

On success the response is at least 0x1C bytes: `u16 capability`, `u16 status (0 = success)`, `u16 AID`, supported
rates IE. A failed association returns a short response with a non-zero status after about two seconds.

## 6. Joining a network

Open network: scan (directed, by SSID), then `0x28`, `0x1C` (value 3), `0x11` authenticate, `0x12` associate
(capability 0x21), `0x28`, `0x7D`.

WPA2-PSK (CCMP): scan, `0x28`, `0x2F` on, `0x28`, `0x1C`, `0x30` (RSN IE), `0x11`, `0x12` (capability 0x31). The
RSN IE we send is `30 14 | 01 00 | group suite | 01 00 | 00-0F-AC-04 | 01 00 | 00-0F-AC-02 | 00 00` where the group
suite is copied from the access point's own RSN IE. The pairwise suite **must be CCMP**: the stock driver's flow
produced "use group cipher" (00-0F-AC-00) for our access point and the AP silently ignored the association.

After the association response the access point sends EAPOL-Key message 1 as an ordinary data frame
(ethertype 0x888E, delivered through port 0x102 like any other frame). The host runs the 4-way handshake in software
(PBKDF2 -> PMK, PRF-384 -> PTK, HMAC-SHA1 MIC, AES key unwrap for the GTK; see `tools/pnet/wpa.c`) and exchanges
messages 2 and 4 as plain data frames. After message 4, load the keys: TK with cmd 0x34 action 3, GTK with cmd 0x35 action 5.
From then on the chip encrypts and decrypts: nothing in the TX/RX frame format changes.

**The RSN IE in message 2 must match the one in the association request byte for byte.**

## 7. Data frames

TX (port 0x102, reg 0x42 = padded length): a 24-byte header followed by an Ethernet II frame.

```
[0] 01        [4..7] 00 40 01 00 (constant in every frame we saw)   [8] 0x18 (header length)
[12..13] u16 length of the Ethernet frame      [14..19] destination MAC      all other bytes 0
```

RX: when reg 0x54 bit 1 is set (or reg 0x40 is non-zero), read that many bytes (rounded up to 4) from port 0x102.
The block has a 16-byte header (`[4..5]` u16 frame length, `[8]` header length = 0x10) followed by an **802.3 frame with
LLC/SNAP** (`dst src length AA AA 03 00 00 00 ethertype payload`); convert it to Ethernet II before handing it up.

## 8. Reconnecting

While the firmware believes it is associated, a new `0x12` command is acknowledged without any frame on the air, so the access
point never restarts the handshake. To reconnect: `0x26` (reason 8), `0x24` (reason 3), clear the keys (0x34 actions 0 and 2,
0x35 actions 0, 2, 4, 6), `0x2F` off, then run the normal join again.

## 9. Pitfalls we hit

* DHCP: with the broadcast flag set, access points answer to `ff:ff:ff:ff:ff:ff` and the chip drops those frames unless the
  multicast filter is set; clear the flag so the reply is unicast.
* Interface address `0.0.0.0` / DHCP client must use the real MAC address (the one from command 0x03).
* The EAPOL `key data` of message 3 is AES-wrapped with the KEK; unwrap before looking for the GTK KDE (`dd .. 00 0f ac 01`).
* Do not run floating point code on this kernel (see `TROUBLESHOOTING.md`).
