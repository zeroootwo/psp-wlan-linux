# Speed: the driver tunables

The driver's defaults are the slow, safe timings it was first brought up with. They can be changed at run time:

```
echo "tune t_stop_ms 0" > /proc/psp_wlan
```

| name | default | fast | meaning |
|------|--------:|-----:|---------|
| `t_int_us` | 700 | 20 | pause around chip interrupt status reads (microseconds) |
| `t_stop_ms` | 2 | 0 | wait after a block-transfer STOP command |
| `t_ack_ms` | 2 | 0 | wait after acknowledging a status bit |
| `t_resp_us` | 0 | 200 | settle time before reading a command response |
| `t_poll_ms` | 10 | 4 | receive poll period of the net_device thread |
| `t_idle_ms` | 1 | 0 | idle sleep when nothing is pending |
| `t_fw_us` | 0 | 100 | per-chunk pause during firmware download |
| `t_fw_wait_ms` | 20 | 1 | wait for the chip after each firmware chunk |

`card/net-exp/tune.sh` sets the fast values (firmware download 1.2 s instead of ~20 s, HTTP throughput about 6x, ping
about 14 ms); `card/net-exp/start.sh` tries them first and falls back to `tune-slow.sh` if the firmware or chip init fails.
`cat /proc/psp_wlan` prints counters ("stats") that show where the time goes.

Measured on a PSP-3004 on 2.4 GHz WPA2: power-on to online about 16 s with the fast settings (the XMB and the plugin
account for most of it), TCP download about 3.3 Mbit/s.

## 64 MB of RAM

`kernel/patches/0003-64mb-ram-highmem-start.patch` raises `HIGHMEM_START`, so Linux sees the full 64 MB of the PSP-2000/3000
(`MemTotal` about 62 MB). Without it the kernel stops at 32 MB.
