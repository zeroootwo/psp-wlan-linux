/*
 * pspwave: PSP-3000 WLAN driver (a Marvell-style ARM WLAN chip, probably the 88W8380 family, behind Memory Stick host #2).
 *
 * Copyright (C) 2026 Anastasiia Velikaya
 * Licensed under the GNU General Public License, version 2.
 *
 * Polled (IRQ 8 is deliberately not used).  Everything is driven through
 * /proc/psp_wlan (write a command, read the log back); the firmware images go
 * through /proc/psp_fwh and /proc/psp_fwm.  All protocol constants come from
 * observing the stock Sony driver talking to the chip; see docs/PROTOCOL.md.
 *
 * Bring-up order (see card/start.sh):
 *   go        power up, reset, read the chip signature
 *   init      chip register init
 *   (cat helper image > /proc/psp_fwh, main image > /proc/psp_fwm)
 *   fw        two-stage firmware download
 *   chipinit  host command sequence that follows firmware start-up
 *   link <wpa|open> <ssid>   directed scan + (auth, assoc [+ RSN IE]); prints LINK_OK / LINK_FAIL
 *   netup     register the wlan0 net_device and start the polling thread
 *   netdown   unregister it
 * Debugging helpers: rd/wr <reg> [val], gi, info, scan, cmd, cmdx <id> <len> <bytes>, auth, assoc,
 * dhcp, rx, regs, swap, exp.
 *
 * Do not use floating point in programs that run on this kernel: the first FPU instruction
 * executed from userland freezes the machine (see docs/TROUBLESHOOTING.md).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/jiffies.h>
#include <asm/uaccess.h>
#include <asm/ipl_sdk/psp_compat.h>
#include <asm/ipl_sdk/syscon.h>

#define R32(a)   (*(volatile u32 *)(a))
#define SYNC()   __asm__ __volatile__("sync" : : : "memory")

#define SYSREG_RESET    0xbc10004c
#define SYSREG_BUSCLK   0xbc100050
#define SYSREG_CLK      0xbc100054
#define SYSREG_IO       0xbc100078
#define SYSREG_CLKSEL   0xbc10005c
#define SYSCTL_DELAYSEL 0xbc000050
#define WLAN_MS_IF      1

#define MS2_BASE        0xbd300000
#define MS_CMD          (MS2_BASE + 0x30)
#define MS_DATA         (MS2_BASE + 0x34)
#define MS_STATUS       (MS2_BASE + 0x38)
#define MS_SYS          (MS2_BASE + 0x3c)
#define MS_REG40        (MS2_BASE + 0x40)

#define ST_TIMEOUT      0x0100
#define ST_CRC          0x0200
#define ST_RDY          0x1000
#define ST_INT          0x2000
#define ST_FIFO         0x4000

#define HELPER_MAX      0x900
#define MAIN_MAX        0x17400
#define HELPER_LEN      2140
#define MAIN_LEN        95228

#define LOGSZ 6144
static char logbuf[LOGSZ];
static int  loglen;
static int  powered;
static int  wr_swap = 1;	/* byte-swap every 32-bit word in PIO page writes (required) */

static u8 *fwh, *fwm;
static int fwh_len, fwm_len;

static void logf(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

static void logf(const char *fmt, ...)
{
	va_list ap;
	char line[160];
	int n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n >= (int)sizeof(line))
		n = sizeof(line) - 1;
	printk(KERN_INFO "psp_wlan: %s", line);
	if (loglen + n < LOGSZ - 1) {
		memcpy(logbuf + loglen, line, n);
		loglen += n;
		logbuf[loglen] = 0;
	}
}

static void rmw(u32 reg, u32 mask, u32 val)
{
	unsigned long flags;
	u32 v;

	local_irq_save(flags);
	v = R32(reg);
	v = (v & ~mask) | (val & mask);
	R32(reg) = v;
	SYNC();
	local_irq_restore(flags);
}

static void sysreg_set(u32 reg, u32 bit, int on) { rmw(reg, bit, on ? bit : 0); }
static void msif_reset(int n, int on)   { sysreg_set(SYSREG_RESET,  0x100 << n, on); }
static void msif_busclk(int n, int on)  { sysreg_set(SYSREG_BUSCLK, 0x400 << n, on); }
static void msif_clk(int n, int on)     { sysreg_set(SYSREG_CLK,    0x100 << n, on); }
static void msif_io(int n, int on)      { sysreg_set(SYSREG_IO,     0x10  << n, on); }
static void msif_clksel(int n, u32 v)   { rmw(SYSREG_CLKSEL,   3 << (2 * n), v << (2 * n)); }
static void msif_delaysel(int n, u32 v) { rmw(SYSCTL_DELAYSEL, 7 << (4 * n), v << (4 * n)); }

static int syscon_dword(u32 param, u8 cmd)
{
	unsigned long flags;
	int r;

	local_irq_save(flags);
	r = pspSyscon_tx_dword(param, cmd, 3);
	local_irq_restore(flags);
	return r;
}

/* ---- polled MS-style TPC layer ---- */

static int ms_poll(u32 mask)
{
	int i;

	for (i = 0; i < 200000; i++) {
		if (R32(MS_STATUS) & mask)
			return 0;
		udelay(5);
	}
	return -1;
}

static int ms_begin(int write)
{
	int i;

	R32(MS_SYS) |= 0xa00;
	SYNC();
	for (i = 0; i < 20000 && (R32(MS_SYS) & 0x200); i++)
		udelay(50);
	if (i == 20000)
		return -1;
	if (write)
		R32(MS_SYS) |= 0x100;
	else
		R32(MS_SYS) &= ~0x100;
	SYNC();
	return 0;
}

static int ms_end(int ack)
{
	u32 st;

	if (ms_poll(ST_RDY))
		return -2;
	st = R32(MS_STATUS);
	if (ack) {
		R32(MS_SYS) |= 0x800;
		SYNC();
	}
	return (st & (ST_CRC | ST_TIMEOUT)) ? -3 : 0;
}

static int ms_put(u32 v)
{
	if (ms_poll(ST_FIFO))
		return -1;
	R32(MS_DATA) = v;
	SYNC();
	return 0;
}

static int ms_get(u32 *v)
{
	if (ms_poll(ST_FIFO))
		return -1;
	*v = R32(MS_DATA);
	return 0;
}

static int ms_set_rw_regs(u8 ra, u8 rl, u8 wa, u8 wl)
{
	int r;

	r = ms_begin(1);
	if (r)
		return r;
	if (ms_poll(ST_RDY))
		return -2;
	R32(MS_CMD) = 0x8004;
	SYNC();
	if (ms_put(ra | (rl << 8) | (wa << 16) | ((u32)wl << 24)))
		return -4;
	if (ms_put(0))
		return -4;
	return ms_end(0);
}

static int ms_read_reg(u8 addr, u8 len, u32 *w0)
{
	u32 w1;
	int r;

	r = ms_set_rw_regs(addr, len, 0, 0);
	if (r)
		return r;
	r = ms_begin(0);
	if (r)
		return r;
	if (ms_poll(ST_RDY))
		return -2;
	R32(MS_CMD) = len | 0x4000;
	SYNC();
	if (ms_get(w0) || ms_get(&w1))
		return -4;
	return ms_end(1);
}

static int ms_write_reg(u8 addr, u8 len, u32 w0)
{
	int r;

	r = ms_set_rw_regs(0, 0, addr, len);
	if (r)
		return r;
	r = ms_begin(1);
	if (r)
		return r;
	if (ms_poll(ST_RDY))
		return -2;
	R32(MS_CMD) = len | 0xb000;
	SYNC();
	if (ms_put(w0) || ms_put(0))
		return -4;
	return ms_end(1);
}

static int ms_get_int(u32 *w0)
{
	u32 w1;
	int r;

	r = ms_begin(0);
	if (r)
		return r;
	if (ms_poll(ST_RDY))
		return -2;
	R32(MS_CMD) = 0x7001;
	SYNC();
	if (ms_get(w0) || ms_get(&w1))
		return -4;
	return ms_end(0);
}

static int ms_ex_set_cmd(u32 w0, u32 w1)
{
	int r;

	r = ms_begin(1);
	if (r)
		return r;
	if (ms_poll(ST_RDY))
		return -2;
	R32(MS_CMD) = 0x9007;
	SYNC();
	if (ms_put(w0) || ms_put(w1))
		return -4;
	return ms_end(0);
}

/* PIO page write (TPC 0xA), len <= 0x200 */
static int ms_write_page(const u8 *p, int len)
{
	u32 tmp[0x82];
	int pairs, i, r;

	memset(tmp, 0, sizeof(tmp));
	memcpy(tmp, p, len);
	if (wr_swap) {
		for (i = 0; i < 2 * ((len + 7) >> 3); i++) {
			u32 x = tmp[i];

			tmp[i] = (x << 24) | ((x & 0xff00) << 8) |
				 ((x >> 8) & 0xff00) | (x >> 24);
		}
	}
	r = ms_begin(1);
	if (r)
		return r;
	if (ms_poll(ST_RDY))
		return -2;
	if (R32(MS_STATUS) & (ST_CRC | ST_TIMEOUT))
		return -3;
	R32(MS_CMD) = len | 0xa000;
	SYNC();
	pairs = (len + 7) >> 3;
	for (i = 0; i < pairs; i++)
		if (ms_put(tmp[2 * i]) || ms_put(tmp[2 * i + 1]))
			return -4;
	return ms_end(0);
}

/* what the stock driver does to the hardware for its DMA mode switch: bit0 = read-dma enable,
 * bit1 = write-dma enable (REG40 bits 0x20 / 0x400, 0x10000 = latch) */
static void ms_dma_mode(int mode)
{
	if (mode & 1)
		rmw(MS_REG40, 0x10020, 0x10020);
	else
		rmw(MS_REG40, 0x10020, 0x10000);
	if (mode & 2)
		rmw(MS_REG40, 0x10400, 0x10400);
	else
		rmw(MS_REG40, 0x10400, 0x10000);
}

/* ---- chip register helpers ---- */

static int rd8(u8 a, u8 *v)
{
	u32 w = 0;
	int r = ms_read_reg(a, 1, &w);

	*v = w & 0xff;
	return r;
}

static int rd16(u8 a, u16 *v)
{
	u32 w = 0;
	int r = ms_read_reg(a, 2, &w);

	*v = ((w & 0xff) << 8) | ((w >> 8) & 0xff);	/* big endian on wire */
	return r;
}

static int rd32raw(u8 a, u32 *v)
{
	*v = 0;
	return ms_read_reg(a, 4, v);
}

static int wr8(u8 a, u8 v)   { return ms_write_reg(a, 1, v); }
static int wr16(u8 a, u16 v) { return ms_write_reg(a, 2, ((v & 0xff) << 8) | (v >> 8)); }

static int dbg_budget = 12;

static void dbg_int(const char *tag)
{
	u32 g = 0xff;
	int gr;

	if (dbg_budget <= 0)
		return;
	dbg_budget--;
	gr = ms_get_int(&g);
	logf("   dbg %s: GET_INT -> %d int=%02x STATUS=%08x\n",
	     tag, gr, g & 0xff, R32(MS_STATUS));
}

/* block write to chip address 0x100 ("data port"),  */
static u32 blk_wr_addr = 0x100;

static int blk_write(const u8 *buf, int len)
{
	u32 w0, w1, iv = 0;
	int sent = 0, page, r, spins = 0, first = 1;

	w0 = 0xb4 | ((len >> 8) & 0xff) << 8 | (len & 0xff) << 16 | 0u << 24;
	w1 = ((blk_wr_addr >> 16) & 0xff) | ((blk_wr_addr >> 8) & 0xff) << 8 |
	     (blk_wr_addr & 0xff) << 16;	/* 0x100 -> 0x0100 as before */
	dbg_int("before EX_SET_CMD");
	r = ms_ex_set_cmd(w0, w1);
	if (r) {
		logf("   blk: EX_SET_CMD -> %d\n", r);
		return r;
	}
	dbg_int("after EX_SET_CMD");
	page = len < 0x200 ? len : 0x200;
	for (;;) {
		int t, got = 0;

		/* The STATUS INT bit never shows up in polled mode, so poll
		 * the INT register directly (any of CMDNK/data/0x60 bits). */
		iv = 0;
		r = 0;
		for (t = 0; t < 1500; t++) {
			if (R32(MS_STATUS) & ST_INT) {
				r = ms_get_int(&iv);
				got = 1;
				break;
			}
			r = ms_get_int(&iv);
			if (r)
				break;
			if (iv & 0x71) {
				got = 1;
				break;
			}
			udelay(700);
		}
		iv &= 0xff;
		if (r) {
			logf("   blk: GET_INT -> %d (sent %d/%d)\n", r, sent, len);
			return r;
		}
		if (!got) {
			if (sent == len)
				break;		/* all pages out, chip is quiet */
			logf("   blk: no INT, sent %d/%d STATUS=%08x last=%02x\n",
			     sent, len, R32(MS_STATUS), iv);
			return -10;
		}
		first = 0;
		if (iv & 0x01) {
			logf("   blk: CMDNK, INT=%02x sent %d/%d\n", iv, sent, len);
			return -11;
		}
		if (iv & 0x10) {
			if (sent >= len) {
				logf("   blk: chip wants more than %d\n", len);
				return -12;
			}
			r = ms_write_page(buf + sent, page);
			if (r) {
				logf("   blk: page write -> %d (sent %d)\n", r, sent);
				return r;
			}
			sent += page;
			page = len - sent < 0x200 ? len - sent : 0x200;
			if (sent == len) {
				udelay(500);	/* all pages out: do not wait for another INT */
				break;
			}
			continue;
		}
		if ((iv & 0x60) == 0x60) {
			if (++spins > 300) {
				logf("   blk: stuck INT=%02x\n", iv);
				return -13;
			}
			continue;
		}
		break;
	}
	if (sent != len) {
		logf("   blk: ended early INT=%02x sent %d/%d\n", iv, sent, len);
		return -14;
	}
	/* the stock driver finishes with EX_SET_CMD 0x25 (STOP) */
	r = ms_ex_set_cmd(0x25, 0);
	msleep(2);
	ms_get_int(&iv);
	if (r) {
		logf("   blk: STOP -> %d\n", r);
		return r;
	}
	return 0;
}

/* polled wait until the chip reports reg 0x54 bit2.
 * Sony waits for the INT line between iterations; we poll reg 0x54. */
static int chip_wait_ready(int ms)
{
	u8 s = 0;
	u32 iv = 0;
	int i, t;

	for (i = 0; i < 1; i++) {
		if (rd8(0x54, &s))
			return -1;
		if (s & 4)
			return 0;
		if (wr8(0x56, 4))
			return -1;
		for (t = 0; t < ms / 10 + 1; t++) {
			msleep(10);
			if (ms_get_int(&iv))
				return -3;
			if (rd8(0x54, &s))
				return -1;
			if (s & 4)
				return 0;
		}
	}
	return -4;
}

/*  */
static int send_block(const u8 *buf, int len)
{
	int r;

	r = wr16(0x44, len);
	if (r) {
		logf("   send: wr 0x44 -> %d\n", r);
		return r;
	}
	ms_dma_mode(1);
	r = chip_wait_ready(20);
	if (r) {
		/* the stock driver does the same: on a ready timeout it rewrites reg
		 * 0x44 and sends the block anyway */
		r = wr16(0x44, len);
		if (r) {
			ms_dma_mode(3);
			return r;
		}
	}
	r = blk_write(buf, len);
	if (!r)
		r = wr8(0x54, 0xfb);
	ms_dma_mode(3);
	return r;
}

/* ---- sequences ---- */

static void sysreg_phase_a(void)
{
	msif_reset(WLAN_MS_IF, 1);
	msif_io(WLAN_MS_IF, 0);
	msif_clk(WLAN_MS_IF, 0);
	msif_busclk(WLAN_MS_IF, 0);
	msif_clksel(WLAN_MS_IF, 0);
	msif_delaysel(WLAN_MS_IF, 4);
	msif_reset(WLAN_MS_IF, 0);
}

static void sysreg_phase_d(void)
{
	msif_reset(WLAN_MS_IF, 1);
	msif_clksel(WLAN_MS_IF, 0);
	msif_delaysel(WLAN_MS_IF, 4);
	msif_busclk(WLAN_MS_IF, 1);
	msif_clk(WLAN_MS_IF, 1);
	msif_io(WLAN_MS_IF, 1);
	msif_reset(WLAN_MS_IF, 0);
}

static void log_reset(void)
{
	loglen = 0;
	logbuf[0] = 0;
}

static void run_probe(void)
{
	u32 v;
	int r;

	log_reset();
	logf("start\n");
	logf("A: sysreg MS2 off/reset\n");
	sysreg_phase_a();
	logf("B: syscon WLAN power on (cmd 0x4d)\n");
	r = syscon_dword(1, 0x4d);
	logf("   -> %d\n", r);
	if (r < 0)
		return;
	msleep(20);
	logf("C: chip reset (0x84, wait, 0x04)\n");
	r = syscon_dword(0x84, 0x32);
	msleep(100);
	r |= syscon_dword(0x04, 0x32) << 8;
	logf("   -> %x\n", r);
	logf("D: sysreg MS2 on\n");
	sysreg_phase_d();
	powered = 1;
	logf("E: MS2 host setup\n");
	rmw(MS_SYS, 0x2020, 0x2020);
	rmw(MS_REG40, 0x10420, 0x10420);
	msleep(50);
	logf("   SYS=%08x STATUS=%08x REG40=%08x\n",
	     R32(MS_SYS), R32(MS_STATUS), R32(MS_REG40));
	r = ms_get_int(&v);
	logf("GET_INT -> %d, int=%02x\n", r, v & 0xff);
	r = ms_read_reg(0x22, 1, &v);
	logf("reg 0x22 -> %d, val=%02x\n", r, v & 0xff);
	r = ms_read_reg(0x24, 1, &v);
	logf("reg 0x24 -> %d, val=%02x\n", r, v & 0xff);
	logf("done\n");
}

static void power_off(void)
{
	int r;

	log_reset();
	msif_busclk(WLAN_MS_IF, 0);
	msif_clk(WLAN_MS_IF, 0);
	msif_io(WLAN_MS_IF, 0);
	r = syscon_dword(0, 0x4d);
	logf("off: syscon WLAN power off -> %d\n", r);
	powered = 0;
}

static void regdump(const char *tag);

static void run_fw(void)
{
	u8 sig = 0, s54 = 0, s4a = 0;
	u16 clen = 0, c;
	u32 magic = 0;
	int r, off, n, chunks = 0, i, tries;
	unsigned long t0 = jiffies;

	log_reset();
	dbg_budget = 0;
	logf("fw: start (helper %d/%d, main %d/%d bytes loaded)\n",
	     fwh_len, HELPER_LEN, fwm_len, MAIN_LEN);
	if (!powered) {
		logf("fw: run 'go' first\n");
		return;
	}
	if (fwh_len != HELPER_LEN || fwm_len != MAIN_LEN) {
		logf("fw: image size wrong (need %d and %d)\n", HELPER_LEN, MAIN_LEN);
		return;
	}
	r = rd8(0x22, &sig);
	logf("fw: reg 0x22 -> %d val=%02x\n", r, sig);
	if (r || sig != 0x24)
		return;

	/* stage 1: helper in chunks of at most 0x600 */
	for (off = 0; off < HELPER_LEN; off += n) {
		n = HELPER_LEN - off < 0x600 ? HELPER_LEN - off : 0x600;
		r = send_block(fwh + off, n);
		logf("fw: helper off=%d len=%d -> %d\n", off, n, r);
		if (r)
			return;
		msleep(20);
	}

	/* hand-over to the helper */
	ms_dma_mode(1);
	regdump("helper sent");
	r = wr16(0x44, 0);
	if (!r) {
		int rr = chip_wait_ready(10);

		logf("fw: helper end: 0x44=0 / ready -> %d (ignored)\n", rr);
	}
	if (!r) {
		static const u8 zeros[16];

		r = blk_write(zeros, 16);
		logf("fw: 16 zero bytes -> %d\n", r);
	}
	if (!r)
		r = wr8(0x54, 0xfb);
	if (!r)
		r = wr8(0x56, 8);
	ms_dma_mode(3);
	logf("fw: stage1 done -> %d\n", r);
	regdump("stage1 done");
	if (r)
		return;

	/* stage 2: the chip dictates the block length in reg 0x4c */
	for (tries = 0; tries < 300; tries++) {
		r = rd16(0x4c, &clen);
		if (r) {
			logf("fw: rd 0x4c -> %d\n", r);
			return;
		}
		if (clen)
			break;
		msleep(10);
	}
	logf("fw: first reg 0x4c = %04x after %d polls\n", clen, tries);
	if (!clen)
		return;

	off = 0;
	c = clen;
	for (;;) {
		if (c & 1) {
			c &= 0xfffe;
			logf("fw: chip asked retry at off=%d (len %d)\n", off, c);
			n = 0;
		} else {
			n = c;
			if (n > MAIN_LEN - off)
				n = MAIN_LEN - off;
			if (n == 0)
				break;
			r = send_block(fwm + off, n);
			if (r) {
				logf("fw: main off=%d len=%d -> %d\n", off, n, r);
				return;
			}
			off += n;
			chunks++;
			if (chunks <= 3 || (chunks % 32) == 0)
				logf("fw: main off=%d/%d (chunk %d len %d)\n",
				     off, MAIN_LEN, chunks, n);
			if (off >= MAIN_LEN)
				break;
		}
		for (tries = 0; tries < 1000; tries++) {
			r = rd16(0x4c, &c);
			if (r) {
				logf("fw: rd 0x4c -> %d\n", r);
				return;
			}
			if (c)
				break;
			msleep(1);
		}
		if (!c) {
			logf("fw: reg 0x4c stayed 0 at off=%d\n", off);
			break;
		}
	}
	logf("fw: stage2 sent %d/%d bytes in %d chunks\n", off, MAIN_LEN, chunks);

	for (i = 0; i < 256; i++) {
		r = rd32raw(0x4a, &magic);
		if (r) {
			logf("fw: rd 0x4a -> %d\n", r);
			return;
		}
		if (magic == 0x46554755)
			break;
		msleep(10);
	}
	rd8(0x54, &s54);
	rd8(0x4a, &s4a);
	logf("fw: reg 0x4a = %08x after %d polls (want 46554755) reg54=%02x\n",
	     magic, i, s54);
	logf("fw: %s (%lu ms)\n", magic == 0x46554755 ? "FIRMWARE RUNNING" : "no ready magic",
	     (jiffies - t0) * 1000UL / HZ);
}

/* ---- experiments: which form of the block-write command does the chip accept? ---- */

static void ex_try(const char *tag, u8 cmd, u16 len, u32 addr)
{
	u32 w0, w1, g = 0xff;
	int r, gr;

	ms_get_int(&g);		/* clear INT */
	w0 = cmd | ((len >> 8) & 0xff) << 8 | (len & 0xff) << 16 | ((addr >> 24) & 0xff) << 24;
	w1 = ((addr >> 16) & 0xff) | ((addr >> 8) & 0xff) << 8 | (addr & 0xff) << 16;
	r = ms_ex_set_cmd(w0, w1);
	msleep(5);
	g = 0xff;
	gr = ms_get_int(&g);
	logf("%s: r=%d int=%02x(%d) st=%04x %s\n", tag, r, g & 0xff, gr,
	     R32(MS_STATUS) & 0xffff, (gr == 0 && r == 0 && !(g & 1)) ? "ACCEPTED" : "nak");
	if (gr == 0 && r == 0 && !(g & 1)) {
		ms_ex_set_cmd(0x25, 0);		/* STOP */
		msleep(5);
		ms_get_int(&g);
	}
}

static void regdump(const char *tag)
{
	u16 a = 0, b = 0, c = 0, d = 0, e = 0;
	u8 r54 = 0, r56 = 0, r22 = 0;
	u32 m = 0;

	rd16(0x40, &a);
	rd16(0x42, &b);
	rd16(0x44, &c);
	rd16(0x46, &d);
	rd16(0x4c, &e);
	rd8(0x54, &r54);
	rd8(0x56, &r56);
	rd8(0x22, &r22);
	rd32raw(0x4a, &m);
	logf("%s: 40=%04x 42=%04x 44=%04x 46=%04x 4c=%04x 54=%02x 56=%02x 22=%02x 4a=%08x\n",
	     tag, a, b, c, d, e, r54, r56, r22, m);
}

static void run_exp(void)
{
	u32 g;

	log_reset();
	if (!powered) {
		logf("exp: run 'go' first\n");
		return;
	}
	ms_get_int(&g);
	regdump("start");
	ms_dma_mode(1);
	wr16(0x44, 0x600);
	regdump("after wr44=600");
	ex_try("k B2 len40 @0 (chip info)", 0xb2, 0x40, 0x0);
	ex_try("a B4 len600 @100", 0xb4, 0x600, 0x100);
	ex_try("b B4 len600 @100 again", 0xb4, 0x600, 0x100);
	ex_try("c B4 len10 @100", 0xb4, 0x10, 0x100);
	ex_try("d B4 len600 @0", 0xb4, 0x600, 0x0);
	ex_try("e B2 len8 @100 (read)", 0xb2, 8, 0x100);
	ex_try("f B3 len8 @100 (read)", 0xb3, 8, 0x100);
	wr16(0x44, 0);
	ex_try("g B4 len600 44=0", 0xb4, 0x600, 0x100);
	wr8(0x56, 4);
	regdump("after wr56=4");
	ex_try("h B4 len600 after 56=4", 0xb4, 0x600, 0x100);
	ex_try("i wake 12", 0x12, 0, 0);
	msleep(500);
	ex_try("j B4 len600 after wake", 0xb4, 0x600, 0x100);
	regdump("end");
	ms_dma_mode(3);
}

/* The stock driver's HAL thread does this chip register init that must
 * happen before the chip accepts any block command.  Verbose, stops at the
 * first failing step so we can see where the bus dies. */
#define ISTEP(tag, expr) do { \
		r = (expr); \
		logf("%s r=%d st=%04x\n", tag, r, R32(MS_STATUS) & 0xffff); \
		if (r) \
			return r; \
	} while (0)

static int hal_init(void)
{
	u8 v = 0;
	u32 g = 0;
	int i, r;

	log_reset();
	if (!powered) {
		logf("init: run 'go' first\n");
		return -1;
	}
	ISTEP("1 get_int", ms_get_int(&g));
	logf("  int=%02x\n", g & 0xff);
	ISTEP("2 rd22", rd8(0x22, &v));
	logf("  22=%02x\n", v);
	ISTEP("3 wr10=7f", wr8(0x10, 0x7f));
	/* the stock driver then puts the chip in fast mode, switch the
	 * host to match: SYS &= ~0x4010, MS2 clksel = 1; delaysel comes from an
	 * unknown config, so scan it and keep the first value that works. */
	rmw(MS_SYS, 0x4010, 0);
	msif_clksel(WLAN_MS_IF, 1);
	{
		static const u8 dly[] = { 4, 0, 1, 2, 3, 5, 6, 7 };
		int k, ok = 0;

		for (k = 0; k < 8; k++) {
			msif_delaysel(WLAN_MS_IF, dly[k]);
			udelay(200);
			v = 0;
			r = rd8(0x22, &v);
			logf("4 dly=%d rd22 r=%d v=%02x st=%04x\n", dly[k], r, v,
			     R32(MS_STATUS) & 0xffff);
			if (!r && v == 0x24) {
				ok = 1;
				break;
			}
		}
		if (!ok) {
			logf("4 no delaysel works, SYS=%08x\n", R32(MS_SYS));
			return -1;
		}
	}
	ISTEP("4b rd10", rd8(0x10, &v));
	logf("  10=%02x\n", v);
	ISTEP("5 rd25", rd8(0x25, &v));
	logf("  25=%02x\n", v);
	ISTEP("6 wr25|=1", wr8(0x25, v | 1));
	for (i = 0; i < 20; i++) {
		r = rd8(0x25, &v);
		if (r || (v & 1))
			break;
		msleep(10);
	}
	logf("7 rd25 r=%d v=%02x polls=%d st=%04x\n", r, v, i,
	     R32(MS_STATUS) & 0xffff);
	if (r)
		return r;
	ISTEP("8 wr26=1", wr8(0x26, 1));
	ISTEP("9 rd27", rd8(0x27, &v));
	logf("  27=%02x\n", v);
	ISTEP("a rd23", rd8(0x23, &v));
	logf("  23=%02x\n", v);
	if (v & 0x80)
		ISTEP("b wr28=1", wr8(0x28, 1));
	msleep(20);
	g = 0xff;
	ISTEP("c get_int", ms_get_int(&g));
	logf("  int=%02x\n", g & 0xff);
	return 0;
}

/* PIO page read (TPC 0x5),  */
static int ms_read_page(u8 *p, int len)
{
	u32 tmp[0x82];
	int pairs, i, r;

	pairs = (len >> 3) + ((len & 7) != 0);
	r = ms_begin(0);
	if (r)
		return r;
	if (ms_poll(ST_RDY))
		return -2;
	if (R32(MS_STATUS) & (ST_CRC | ST_TIMEOUT))
		return -3;
	R32(MS_CMD) = len | 0x5000;
	SYNC();
	for (i = 0; i < pairs; i++)
		if (ms_get(&tmp[2 * i]) || ms_get(&tmp[2 * i + 1]))
			return -4;
	r = ms_end(0);
	if (r)
		return r;
	if (wr_swap) {
		for (i = 0; i < 2 * pairs; i++) {
			u32 x = tmp[i];

			tmp[i] = (x << 24) | ((x & 0xff00) << 8) |
				 ((x >> 8) & 0xff00) | (x >> 24);
		}
	}
	memcpy(p, tmp, len);
	return 0;
}

/* block read from chip address addr (cmd 0xB2) */
static int blk_read(u8 *buf, int len, u32 addr, u8 cmdb)
{
	u32 w0, w1, iv = 0;
	int got = 0, page, r, t, ev;

	w0 = cmdb | ((len >> 8) & 0xff) << 8 | (len & 0xff) << 16 |
	     ((addr >> 24) & 0xff) << 24;
	w1 = ((addr >> 16) & 0xff) | ((addr >> 8) & 0xff) << 8 | (addr & 0xff) << 16;
	r = ms_ex_set_cmd(w0, w1);
	if (r) {
		logf("   rd: EX_SET_CMD -> %d\n", r);
		return r;
	}
	page = len < 0x200 ? len : 0x200;
	while (got < len) {
		ev = 0;
		for (t = 0; t < 1500; t++) {
			r = ms_get_int(&iv);
			if (r) {
				logf("   rd: GET_INT -> %d\n", r);
				return r;
			}
			if (iv & 0x71) {
				ev = 1;
				break;
			}
			udelay(700);
		}
		iv &= 0xff;
		if (!ev) {
			logf("   rd: no INT, got %d/%d\n", got, len);
			return -10;
		}
		if (iv & 1) {
			logf("   rd: CMDNK int=%02x got %d/%d\n", iv, got, len);
			return -11;
		}
		if (iv & 0x10) {
			r = ms_read_page(buf + got, page);
			if (r) {
				logf("   rd: page -> %d (got %d)\n", r, got);
				return r;
			}
			got += page;
			page = len - got < 0x200 ? len - got : 0x200;
			continue;
		}
		logf("   rd: unexpected int=%02x got %d/%d\n", iv, got, len);
		return -12;
	}
	ms_ex_set_cmd(0x25, 0);
	msleep(2);
	ms_get_int(&iv);
	return 0;
}

static void run_info(void)
{
	u8 b[0x40];
	int r, i;

	log_reset();
	if (!powered) {
		logf("info: run 'go' and 'init' first\n");
		return;
	}
	memset(b, 0xee, sizeof(b));
	ms_dma_mode(0);
	r = blk_read(b, 0x40, 0, 0xb2);
	ms_dma_mode(3);
	logf("info: B2 read 0x40 @0 -> %d\n", r);
	for (i = 0; i < 0x40; i += 16)
		logf("%02x: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
		     i, b[i], b[i+1], b[i+2], b[i+3], b[i+4], b[i+5], b[i+6], b[i+7],
		     b[i+8], b[i+9], b[i+10], b[i+11], b[i+12], b[i+13], b[i+14], b[i+15]);
	regdump("after info");
}

/* ---- talk to the running firmware (Marvell-style host command) ---- */

static u16 cmd_seq;
static u8 rbuf[0xc10];
static u8 psp_mac[6] = { 0x02, 0x50, 0x53, 0x50, 0x00, 0x01 };	/* replaced by the real one from cmd 0x03 */

/* read one pending response (reg 0x54 bit0 set) into rbuf; returns its length */
static int fw_read_resp(u16 *rlenp, int quiet)
{
	u16 rlen = 0;
	int r = 0;

	rd16(0x4a, &rlen);
	*rlenp = rlen;
	if (rlen < 8 || rlen > 0xc00) {
		if (!quiet)
			logf("resp: bad length %04x\n", rlen);
		return -21;
	}
	memset(rbuf, 0, sizeof(rbuf));
	ms_dma_mode(0);
	r = blk_read(rbuf, (rlen + 3) & ~3, 0x100, 0xb3);
	ms_dma_mode(3);
	if (!r && rlen >= 0x16 && rbuf[0] == 0x03 && rbuf[1] == 0x80)
		memcpy(psp_mac, rbuf + 16, 6);	/* the chip reports its own MAC here */
	return r;
}

/* send one host command block and read its response into rbuf */
static int fw_xfer(const u8 *cb, int len, u16 *rlenp, int tmo)
{
	u16 rlen = 0;
	int r, t, k;
	u8 s54 = 0;

	*rlenp = 0;
	/* drain stale responses so replies cannot lag one command behind */
	for (k = 0; k < 4; k++) {
		rd8(0x54, &s54);
		if (!(s54 & 0x10))
			break;
		r = fw_read_resp(&rlen, 1);
		wr8(0x56, 0x10);
		msleep(2);
		logf("drained stale resp len=%04x (%d) cmd=%04x\n", rlen, r,
		     rbuf[0] | rbuf[1] << 8);
	}
	ms_dma_mode(1);
	r = wr16(0x44, len);
	if (!r)
		r = blk_write(cb, len);
	if (!r)
		r = wr8(0x54, 0xfb);
	ms_dma_mode(0);
	if (r) {
		logf("send -> %d\n", r);
		return r;
	}
	for (t = 0; t < tmo; t++) {
		rd8(0x54, &s54);
		if (s54 & 0x10)
			break;
		msleep(10);
	}
	if (!(s54 & 0x10)) {
		logf("no response, reg54=%02x after %d polls\n", s54, t);
		return -20;
	}
	r = fw_read_resp(&rlen, 0);
	wr8(0x56, 0x10);		/* ack bit4 */
	msleep(2);
	rd8(0x54, &s54);
	logf("resp len=%04x after %d polls, read -> %d, reg54 after ack=%02x\n",
	     rlen, t, r, s54);
	*rlenp = rlen;
	return r;
}

static void hexdump(const u8 *b, int n)
{
	int i;

	for (i = 0; i < n; i += 16) {
		int k, m = n - i < 16 ? n - i : 16;
		char line[64];
		int o = 0;

		for (k = 0; k < m; k++)
			o += sprintf(line + o, "%02x%s", b[i + k],
				     (k & 3) == 3 ? " " : "");
		logf("%03x: %s\n", i, line);
	}
}

static void show_resp(u16 rlen, int maxdump)
{
	logf("hdr cmd=%04x size=%04x seq=%04x result=%04x\n",
	     rbuf[0] | rbuf[1] << 8, rbuf[2] | rbuf[3] << 8,
	     rbuf[4] | rbuf[5] << 8, rbuf[6] | rbuf[7] << 8);
	hexdump(rbuf, rlen < maxdump ? rlen : maxdump);
}

static void fill_hdr(u8 *cb, u16 id, u16 len)
{
	cmd_seq++;
	cb[0] = id & 0xff;
	cb[1] = id >> 8;
	cb[2] = len & 0xff;
	cb[3] = len >> 8;
	cb[4] = cmd_seq & 0xff;
	cb[5] = cmd_seq >> 8;
}

static void run_fwcmd(u16 id, u16 len)
{
	static u8 cb[0x200];
	u16 rlen;
	int r;

	log_reset();
	if (len < 8 || len > 0x1f0) {
		logf("cmd: bad len\n");
		return;
	}
	memset(cb, 0, sizeof(cb));
	fill_hdr(cb, id, len);
	r = fw_xfer(cb, len, &rlen, 300);
	logf("cmd %04x len %04x -> %d\n", id, len, r);
	if (!r)
		show_resp(rlen, 0x60);
}

static u8 scan_bssid[8][6];
static u16 scan_cap[8];
static int scan_cnt;
static char scan_ssid[8][33];
static u8 scan_ch[8];
static u16 scan_bi[8];
static int open_idx = -1;	/* first scan entry without the privacy bit */

static void parse_scan(u16 rlen)
{
	int nr, off, i;

	if (rlen < 0x21 || (rbuf[0] | rbuf[1] << 8) != 0x8006)
		return;
	nr = rbuf[0x20];
	scan_cnt = 0;
	open_idx = -1;
	off = 0x21;
	logf("scan: %d bss, bytes=%d\n", nr, rbuf[0x1e] | rbuf[0x1f] << 8);
	for (i = 0; i < nr; i++) {
		u8 *e;
		int len, ielen, k, ch = 0;
		char ssid[33];
		int sl = 0, sec = 0;
		u16 cap;
		u8 *ie;

		if (off + 2 > rlen)
			break;
		len = rbuf[off] | rbuf[off + 1] << 8;
		if (len < 18 || off + 2 + len > rlen)
			break;
		e = rbuf + off + 2;
		cap = e[16] | e[17] << 8;
		if (i < 8) {
			memcpy(scan_bssid[i], e, 6);
			scan_cap[i] = cap;
			scan_cnt = i + 1;
			if (!(cap & 0x10) && open_idx < 0)
				open_idx = i;
		}
		ie = e + 18;
		ielen = len - 18;
		ssid[0] = 0;
		for (k = 0; k + 2 <= ielen;) {
			int t = ie[k], l = ie[k + 1];

			if (k + 2 + l > ielen)
				break;
			if (t == 0) {
				int m;

				sl = l > 32 ? 32 : l;
				for (m = 0; m < sl; m++)
					ssid[m] = (ie[k + 2 + m] >= 32 && ie[k + 2 + m] < 127) ?
						  ie[k + 2 + m] : '.';
				ssid[sl] = 0;
			} else if (t == 3 && l >= 1) {
				ch = ie[k + 2];
			} else if (t == 0x30) {
				sec |= 2;		/* RSN (WPA2) */
			} else if (t == 0xdd && l >= 4 && ie[k + 2] == 0x00 &&
				   ie[k + 3] == 0x50 && ie[k + 4] == 0xf2 &&
				   ie[k + 5] == 0x01) {
				sec |= 1;		/* WPA */
			}
			k += 2 + l;
		}
		if (i < 8) {
			memcpy(scan_ssid[i], ssid, sizeof(ssid));
			scan_ch[i] = ch;
			scan_bi[i] = e[14] | e[15] << 8;
		}
		logf("%d %02x:%02x:%02x:%02x:%02x:%02x ch%d %s%s%s '%s'\n", i,
		     e[0], e[1], e[2], e[3], e[4], e[5], ch,
		     (cap & 0x10) ? "priv " : "open ", (sec & 2) ? "WPA2 " : "",
		     (sec & 1) ? "WPA" : "", ssid);
		off += 2 + len;
	}
	if (off < rlen)
		hexdump(rbuf + off, rlen - off < 32 ? rlen - off : 32);
}

/* scan: same 0x48-byte command  builds (default branch) */
static void run_scan(int flag)
{
	static u8 cb[0x60];
	u16 rlen;
	int r, i;

	log_reset();
	memset(cb, 0, sizeof(cb));
	fill_hdr(cb, 6, 0x48);
	cb[8] = flag != 0;
	cb[10] = 3;			/* bss type: any */
	memset(cb + 11, 0xff, 6);	/* bssid: any */
	/* [17..48] ssid = 32 zero bytes */
	cb[49] = 0;
	cb[50] = 0x14;			/* u16 LE 0x0014 */
	for (i = 0; i < 11; i++)
		cb[52 + i] = i + 1;	/* channels 1..11, rest 0 */
	cb[66] = 0x0c;			/* u16 LE 0x000c */
	cb[68] = 0x18;			/* u16 LE 0x0018 */
	r = fw_xfer(cb, 0x48, &rlen, 600);
	logf("scan flag=%d -> %d\n", flag, r);
	if (!r) {
		show_resp(rlen, 0x10);
		parse_scan(rlen);
	}
}

/* cmd 0x11 authenticate (): bssid[6] @8, u8 @14 = shared-key flag, u16 LE @15 */
static void run_auth(int idx, int type)
{
	static u8 cb[0x20];
	u16 rlen;
	int r;

	log_reset();
	if (idx == 99)
		idx = open_idx;
	if (idx < 0 || idx >= scan_cnt) {
		logf("auth: no scan entry %d (have %d)\n", idx, scan_cnt);
		return;
	}
	memset(cb, 0, sizeof(cb));
	fill_hdr(cb, 0x11, 0x11);
	memcpy(cb + 8, scan_bssid[idx], 6);
	cb[14] = type == 1;
	r = fw_xfer(cb, 0x14, &rlen, 600);
	logf("auth idx=%d type=%d -> %d\n", idx, type, r);
	if (!r)
		show_resp(rlen, 0x20);
}

/* cmd 0x12 associate: bssid[6] @8, u16 @14 (cap), u16 @16, u16 @18, u32 @20 - meaning of last three unknown */
static void run_assoc(int idx, u16 a14, u16 a16, u16 a18, u32 a20)
{
	static u8 cb[0x20];
	u16 rlen;
	int r;

	log_reset();
	if (idx == 99)
		idx = open_idx;
	if (idx >= 0 && idx < scan_cnt && a14 == 0xffff)
		a14 = scan_cap[idx];	/* ffff = use capability from scan */
	if (idx < 0 || idx >= scan_cnt) {
		logf("assoc: no scan entry %d (have %d)\n", idx, scan_cnt);
		return;
	}
	memset(cb, 0, sizeof(cb));
	fill_hdr(cb, 0x12, 0x18);
	memcpy(cb + 8, scan_bssid[idx], 6);
	cb[14] = a14; cb[15] = a14 >> 8;
	cb[16] = a16; cb[17] = a16 >> 8;
	cb[18] = a18; cb[19] = a18 >> 8;
	cb[20] = a20; cb[21] = a20 >> 8; cb[22] = a20 >> 16; cb[23] = a20 >> 24;
	r = fw_xfer(cb, 0x18, &rlen, 1000);
	logf("assoc idx=%d %04x %04x %04x %08x -> %d\n", idx, a14, a16, a18, a20, r);
	if (!r)
		show_resp(rlen, 0x20);
}

/* ---- data path: TX = reg 0x42 len + block write @0x102; RX = reg 0x40 len + block read @0x102 ---- */
static int tx_variant;

static int data_tx(const u8 *f, int len)
{
	static u8 tb[0x400];
	int al = (len + 3) & ~3, r;

	memset(tb, 0, al);
	memcpy(tb, f, len);
	ms_dma_mode(1);
	r = wr16(0x42, al);
	blk_wr_addr = 0x102;
	if (!r)
		r = blk_write(tb, al);
	blk_wr_addr = 0x100;
	if (!r && tx_variant) {
		static const u8 poke[7] = { 0, 0xfb, 0xfd, 0xfe, 0xef, 0xf7, 0xdf };

		if (tx_variant < 7)
			r = wr8(0x54, poke[tx_variant]);	/* poke host->card event like after a command */
	}
	ms_dma_mode(0);
	msleep(300);
	regdump("after tx +300ms");
	logf("data_tx len=%d variant=%d -> %d\n", len, tx_variant, r);
	return r;
}

/* listen for up to ms milliseconds, log data frames (reg 0x40) and async firmware events (reg 0x54 bit4) */
static void data_listen(int ms)
{
	static u8 pb[0x800];
	int t, shown = 0;

	for (t = 0; t < ms / 10 && shown < 12; t++) {
		u16 l = 0, rl;
		u8 s54 = 0;

		rd8(0x54, &s54);
		if ((s54 & 0x0b) && shown < 12) {
			logf("t=%d reg54=%02x\n", t * 10, s54);
			if (s54 & 0x08) {		/* async event: code in reg 0x4e, ack with 0xf7 (Sony) */
				u32 ev = 0;

				rd32raw(0x4e, &ev);
				wr8(0x54, 0xf7);
				logf("t=%d EVENT code=%08x\n", t * 10, ev);
			}
		}
		if (s54 & 0x10) {
			int r = fw_read_resp(&rl, 1);

			wr8(0x56, 0x10);
			logf("t=%d EVENT len=%04x (%d)\n", t * 10, rl, r);
			hexdump(rbuf, rl < 0x40 ? rl : 0x40);
			shown++;
		}
		rd16(0x40, &l);
		if (l >= 0x18 && l < 0x7f0) {
			int al = (l + 3) & ~3, r;

			memset(pb, 0, al);
			ms_dma_mode(0);
			r = blk_read(pb, al, 0x102, 0xb3);
			ms_dma_mode(3);
			logf("t=%d RX len=%04x (%d)\n", t * 10, l, r);
			hexdump(pb, l < 0x70 ? l : 0x70);
			shown++;
		}
		msleep(10);
	}
	logf("listen done, %d items\n", shown);
}

static u16 csum16(const u8 *b, int n)
{
	u32 s = 0;
	int i;

	for (i = 0; i + 1 < n; i += 2)
		s += b[i] << 8 | b[i + 1];
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return ~s;
}

/* broadcast DHCP DISCOVER. TX buffer = 24-byte header (seen in Sony's log) + ethernet II frame:
 *   [0]=1 [4]=00 40 01 00 [8]=0x18 (header len) [12..13]=frame len LE [14..19]=dst MAC [20..23]=0 */
static int listen_ms = 15000;

static void run_dhcp(void)
{
	static u8 f[24 + 14 + 20 + 8 + 240 + 8];
	const u8 *mac = psp_mac;
	int n = 14 + 20 + 8 + 240 + 8, ip = 24 + 14, udp = 24 + 34, bp = 24 + 42, o;
	u8 *e = f + 24;
	u16 c;

	log_reset();
	memset(f, 0, sizeof(f));
	f[0] = 1;
	f[5] = 0x40;
	f[6] = 1;
	f[8] = 0x18;
	f[12] = n;
	f[13] = n >> 8;
	memset(f + 14, 0xff, 6);
	memset(e, 0xff, 6);
	memcpy(e + 6, mac, 6);
	e[12] = 0x08;
	e[13] = 0x00;
	f[ip] = 0x45;
	f[ip + 2] = (n - 14) >> 8;
	f[ip + 3] = (n - 14) & 0xff;
	f[ip + 8] = 64;
	f[ip + 9] = 17;
	memset(f + ip + 16, 0xff, 4);		/* dst 255.255.255.255, src 0.0.0.0 */
	c = csum16(f + ip, 20);
	f[ip + 10] = c >> 8;
	f[ip + 11] = c;
	f[udp] = 0; f[udp + 1] = 68;
	f[udp + 2] = 0; f[udp + 3] = 67;
	f[udp + 4] = (n - 34) >> 8;
	f[udp + 5] = (n - 34) & 0xff;
	f[bp] = 1; f[bp + 1] = 1; f[bp + 2] = 6;
	f[bp + 4] = 0x50; f[bp + 5] = 0x53; f[bp + 6] = 0x50; f[bp + 7] = 0x01;
	/* flags = 0 (unicast reply), as in Sony's DISCOVER */
	memcpy(f + bp + 28, mac, 6);
	o = bp + 236;
	f[o++] = 99; f[o++] = 130; f[o++] = 83; f[o++] = 99;
	f[o++] = 53; f[o++] = 1; f[o++] = 1;	/* DISCOVER */
	f[o++] = 255;
	logf("dhcp: mac %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	data_tx(f, 24 + n);
	data_listen(listen_ms);
}

static void run_rx(int sec)
{
	log_reset();
	data_listen((sec > 0 ? sec : 10) * 1000);
}

/* cmd 0x2b = Sony ioctl 7: BSS join/config. SSID[32]@8, type@40 (1=infra), beacon u16@41,
 * dtim@43, IBSS-param IE @44 (6,2,atim), DS-param IE @52 (3,1,channel), capability u16@59, basic rates @61 */
static void run_join(int idx, int rates)
{
	static u8 cb[0x60];
	u16 rlen;
	int r;

	log_reset();
	if (idx == 99)
		idx = open_idx;
	if (idx < 0 || idx >= scan_cnt) {
		logf("join: no scan entry %d (have %d)\n", idx, scan_cnt);
		return;
	}
	memset(cb, 0, sizeof(cb));
	fill_hdr(cb, 0x2b, 0x4f);
	strncpy((char *)cb + 8, scan_ssid[idx], 32);
	cb[40] = 1;
	cb[41] = scan_bi[idx];
	cb[42] = scan_bi[idx] >> 8;
	cb[43] = 1;
	cb[44] = 6;
	cb[45] = 2;
	cb[52] = 3;
	cb[53] = 1;
	cb[54] = scan_ch[idx];
	cb[59] = scan_cap[idx];
	cb[60] = scan_cap[idx] >> 8;
	cb[61] = rates;
	r = fw_xfer(cb, 0x50, &rlen, 1000);
	logf("join idx=%d '%s' ch%d bi=%d cap=%04x rates=%02x -> %d\n", idx, scan_ssid[idx],
	     scan_ch[idx], scan_bi[idx], scan_cap[idx], rates, r);
	if (!r)
		show_resp(rlen, 0x20);
}


/* ================= chip init + link (replaces the hand-written cmdx replay in auto.sh) =================
 * Every constant below comes from traces of the Sony driver talking to the chip (see docs/PROTOCOL.md). */
struct ccmd {
	u8 id;
	u16 size;
	u8 n;
	u8 pay[16];
};

static const struct ccmd init_cmds[] = {
	{ 0x03, 0x2e, 14, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } },
	{ 0x20, 0xc, 3, { 0x01, 0x00, 0x01 } },
	{ 0x20, 0xc, 3, { 0x02, 0x00, 0x01 } },
	{ 0x1c, 0xc, 3, { 0x01, 0x00, 0x01 } },
	{ 0x1e, 0xc, 3, { 0x01, 0x00, 0x0a } },
	{ 0x22, 0x14, 0, { 0 } },
	{ 0x16, 0x8e, 8, { 0x01, 0x00, 0x05, 0x00, 0x02, 0x00, 0x2a, 0x09 } },
	{ 0x16, 0x8e, 8, { 0x01, 0x00, 0x08, 0x00, 0x02, 0x00, 0x2a, 0x09 } },
	{ 0x16, 0x8e, 7, { 0x01, 0x00, 0x06, 0x00, 0x02, 0x00, 0x04 } },
	{ 0x7a, 0x10, 7, { 0x01, 0x00, 0x01, 0x00, 0x0a, 0x00, 0x08 } },
	{ 0x28, 0xc, 1, { 0x63 } },
};

/* send one fw command (payload at offset 8, zero padded); the response stays in rbuf */
static int send_cmd(u16 id, u16 size, const u8 *pay, int n)
{
	static u8 cb[0x200];
	u16 rl;

	memset(cb, 0, sizeof(cb));
	fill_hdr(cb, id, size);
	if (n)
		memcpy(cb + 8, pay, n);
	return fw_xfer(cb, (size + 3) & ~3, &rl, 600);
}

static void run_chipinit(void)
{
	int i, r;

	log_reset();
	wr8(0x5e, 0x7f);		/* event masks, Sony does this right after fw ready */
	wr8(0x5c, 0x7f);
	wr8(0x5a, 0x7f);
	for (i = 0; i < (int)(sizeof(init_cmds) / sizeof(init_cmds[0])); i++) {
		r = send_cmd(init_cmds[i].id, init_cmds[i].size, init_cmds[i].pay, init_cmds[i].n);
		if (r) {
			logf("CHIPINIT_FAIL cmd %02x -> %d\n", init_cmds[i].id, r);
			return;
		}
	}
	logf("CHIPINIT_OK mac=%02x:%02x:%02x:%02x:%02x:%02x\n", psp_mac[0], psp_mac[1], psp_mac[2],
	     psp_mac[3], psp_mac[4], psp_mac[5]);
}


/* Tear down a previous association: the firmware answers "ok" to a new assoc without any frame on the
 * air while it still believes it is associated, so a reconnect needs disassoc + deauth first (cmds 0x26 /
 * 0x24: bssid + reason u16, from Sony's ioctls 4 / 0xb) and the old keys must be cleared (cmd 0x34 actions
 * 0/2, cmd 0x35 actions 0/2/4/6 = the "set" actions minus one) or the chip refuses the plain EAPOL frames. */
static u8 last_bssid[6];
static int link_dirty;

static void link_teardown(void)
{
	static const u8 act[4][2] = { { 0, 0 }, { 2, 0 }, { 4, 0 }, { 6, 0 } };
	u8 pay[16];
	int i;

	if (!link_dirty)
		return;
	memset(pay, 0, sizeof(pay));
	memcpy(pay, last_bssid, 6);
	pay[6] = 8;				/* reason 8: leaving the BSS */
	send_cmd(0x26, 0x10, pay, 8);
	pay[6] = 3;				/* reason 3: deauthenticated, leaving */
	send_cmd(0x24, 0x10, pay, 8);
	for (i = 0; i < 2; i++)
		send_cmd(0x34, 0x2a, act[i], 2);
	for (i = 0; i < 4; i++)
		send_cmd(0x35, 0x2a, act[i], 2);
	send_cmd(0x2f, 0xc, (const u8 *)"\x01\x00\x00", 3);
	link_dirty = 0;
	msleep(200);
	logf("link: previous association torn down\n");
}

struct bss_pick {
	u8 bssid[6];
	u16 cap;
	int ch;
	int rsn_ok;			/* RSN IE with CCMP pairwise + PSK */
	int has_rsn;
	u8 gsuite;			/* group cipher suite type of the AP */
};

static const u8 oui_rsn[3] = { 0x00, 0x0f, 0xac };

/* inspect an RSN IE value: needs CCMP in the pairwise list and PSK in the AKM list */
static void parse_rsn(const u8 *v, int l, struct bss_pick *b)
{
	int pc, ac, i, p = 8;

	b->has_rsn = 1;
	if (l < 12 || v[0] != 1 || v[1] != 0)
		return;
	b->gsuite = memcmp(v + 2, oui_rsn, 3) ? 0 : v[5];
	pc = v[6] | v[7] << 8;
	if (p + 4 * pc + 2 > l)
		return;
	for (i = 0; i < pc; i++)
		if (!memcmp(v + p + 4 * i, oui_rsn, 3) && v[p + 4 * i + 3] == 4)
			b->rsn_ok |= 1;
	p += 4 * pc;
	ac = v[p] | v[p + 1] << 8;
	p += 2;
	if (p + 4 * ac > l)
		return;
	for (i = 0; i < ac; i++)
		if (!memcmp(v + p + 4 * i, oui_rsn, 3) && v[p + 4 * i + 3] == 2)
			b->rsn_ok |= 2;
}

/* parse the 0x8006 scan response in rbuf and pick the first BSS with this SSID */
static int pick_bss(const char *ssid, u16 rlen, struct bss_pick *b)
{
	int nr, off = 0x21, i, sl = strlen(ssid);

	if (rlen < 0x21 || (rbuf[0] | rbuf[1] << 8) != 0x8006)
		return 0;
	nr = rbuf[0x20];
	for (i = 0; i < nr; i++) {
		const u8 *e, *ie;
		int len, ielen, k, match = 0;
		struct bss_pick t;

		if (off + 2 > rlen)
			break;
		len = rbuf[off] | rbuf[off + 1] << 8;
		if (len < 18 || off + 2 + len > rlen)
			break;
		e = rbuf + off + 2;
		off += 2 + len;
		memset(&t, 0, sizeof(t));
		memcpy(t.bssid, e, 6);
		t.cap = e[16] | e[17] << 8;
		ie = e + 18;
		ielen = len - 18;
		for (k = 0; k + 2 <= ielen;) {
			int ty = ie[k], l = ie[k + 1];

			if (k + 2 + l > ielen)
				break;
			if (ty == 0 && l == sl && !memcmp(ie + k + 2, ssid, sl))
				match = 1;
			else if (ty == 3 && l >= 1)
				t.ch = ie[k + 2];
			else if (ty == 0x30)
				parse_rsn(ie + k + 2, l, &t);
			k += 2 + l;
		}
		if (match) {
			*b = t;
			return 1;
		}
	}
	return 0;
}

/* link <wpa|open> <ssid>: directed scan, then the association sequence Sony uses.
 * Prints LINK_OK ... rsn=<our RSN IE> or LINK_FAIL <reason> (read /proc/psp_wlan). */
static void run_link(char *args)
{
	static u8 cb[0x60];
	struct bss_pick b;
	char ssid[33];
	u8 pay[0x30], rsn[24];
	u16 rl, cap_req, status;
	int secure, i, r, n;
	char *p = args;

	log_reset();
	while (*p == ' ')
		p++;
	secure = !strncmp(p, "wpa", 3);
	while (*p && *p != ' ')
		p++;
	while (*p == ' ')
		p++;
	n = 0;
	while (p[n] && p[n] != '\n' && p[n] != '\r' && n < 32) {
		ssid[n] = p[n];
		n++;
	}
	ssid[n] = 0;
	if (!n) {
		logf("LINK_FAIL no ssid\n");
		return;
	}

	link_teardown();

	/* directed scan, same layout as Sony's: type=1, bssid any, ssid@17, channels 1..13 @52 */
	memset(cb, 0, sizeof(cb));
	fill_hdr(cb, 6, 0x48);
	cb[10] = 1;
	memset(cb + 11, 0xff, 6);
	memcpy(cb + 17, ssid, n);
	for (i = 0; i < 13; i++)
		cb[52 + i] = i + 1;
	cb[66] = 6;
	cb[68] = 0x64;
	r = fw_xfer(cb, 0x48, &rl, 600);
	if (r) {
		logf("LINK_FAIL scan cmd %d\n", r);
		return;
	}
	memset(&b, 0, sizeof(b));
	if (!pick_bss(ssid, rl, &b)) {
		logf("LINK_FAIL ssid '%s' not found\n", ssid);
		return;
	}
	logf("link: found %02x:%02x:%02x:%02x:%02x:%02x ch%d cap=%04x rsn=%d/%d\n", b.bssid[0], b.bssid[1],
	     b.bssid[2], b.bssid[3], b.bssid[4], b.bssid[5], b.ch, b.cap, b.has_rsn, b.rsn_ok);
	if (secure && b.rsn_ok != 3) {
		logf("LINK_FAIL AP has no WPA2-PSK/CCMP (rsn_ok=%d)\n", b.rsn_ok);
		return;
	}
	if (!secure && (b.cap & 0x10)) {
		logf("LINK_FAIL AP is encrypted but no passphrase configured\n");
		return;
	}
	cap_req = 0x21 | (secure ? 0x10 : 0);

	send_cmd(0x28, 0xc, (const u8 *)"\x63", 1);
	if (secure)
		send_cmd(0x2f, 0xc, (const u8 *)"\x01\x00\x01", 3);
	send_cmd(0x28, 0xc, (const u8 *)"\x63", 1);
	send_cmd(0x1c, 0xc, (const u8 *)"\x01\x00\x03", 3);
	if (secure) {
		/* RSN IE: group = AP's group cipher, pairwise CCMP, AKM PSK, caps 0 */
		memcpy(rsn, "\x30\x14\x01\x00\x00\x0f\xac\x00\x01\x00\x00\x0f\xac\x04"
		       "\x01\x00\x00\x0f\xac\x02\x00\x00", 22);
		rsn[7] = b.gsuite ? b.gsuite : 4;
		memset(pay, 0, sizeof(pay));
		pay[0] = 1;
		memcpy(pay + 2, rsn, 22);
		if (send_cmd(0x30, 0x10b, pay, 24)) {
			logf("LINK_FAIL rsn cmd\n");
			return;
		}
	}
	memcpy(last_bssid, b.bssid, 6);
	link_dirty = 1;
	memset(pay, 0, sizeof(pay));
	memcpy(pay, b.bssid, 6);
	if (send_cmd(0x11, 0x11, pay, 9)) {
		logf("LINK_FAIL auth cmd\n");
		return;
	}
	memset(pay, 0, sizeof(pay));
	memcpy(pay, b.bssid, 6);
	pay[8] = cap_req;
	pay[9] = cap_req >> 8;
	pay[10] = 3;
	if (send_cmd(0x12, 0x18, pay, 12)) {
		logf("LINK_FAIL assoc cmd\n");
		return;
	}
	status = rbuf[10] | rbuf[11] << 8;
	if (status || (rbuf[2] | rbuf[3] << 8) < 0x1a) {
		logf("LINK_FAIL assoc refused status=%u size=%u\n", status, rbuf[2] | rbuf[3] << 8);
		return;
	}
	if (!secure) {
		send_cmd(0x28, 0xc, (const u8 *)"\x63", 1);
		send_cmd(0x7d, 0xc, (const u8 *)"\x01\x00\x32", 3);
	}
	{
		char hex[64];

		hex[0] = 0;
		if (secure)
			for (i = 0; i < 22; i++)
				sprintf(hex + 2 * i, "%02x", rsn[i]);
		logf("LINK_OK bssid=%02x:%02x:%02x:%02x:%02x:%02x ch=%d secure=%d gcipher=%d rsn=%s\n",
		     b.bssid[0], b.bssid[1], b.bssid[2], b.bssid[3], b.bssid[4], b.bssid[5], b.ch, secure,
		     secure ? rsn[7] : 0, hex);
	}
}

/* ================= network interface (wlan0) =================
 * Polled: one kernel thread owns the bus while it runs (bus_mutex, shared with
 * the /proc command path).  TX = 24-byte Sony header + ethernet II frame on
 * port 0x102; RX = 16-byte header + 802.3/LLC-SNAP frame, converted to ethernet II.
 * Association must already be done with the proc commands (see auto.sh). */
static DEFINE_MUTEX(bus_mutex);

struct pw_priv {
	struct net_device_stats stats;
};

static struct net_device *pw_dev;
static struct task_struct *pw_task;
static struct sk_buff_head pw_txq;
static DECLARE_WAIT_QUEUE_HEAD(pw_wq);
static u8 pw_txbuf[24 + 1536 + 8] __attribute__((aligned(32)));
static u8 pw_rxbuf[0x800] __attribute__((aligned(32)));
static unsigned long pw_events;

static void pw_rx_frame(const u8 *f, int n)
{
	struct pw_priv *pr = netdev_priv(pw_dev);
	const u8 *pl;
	int plen;
	u16 proto;
	struct sk_buff *skb;
	u8 *e;

	if (n < 14) {
		pr->stats.rx_errors++;
		return;
	}
	proto = f[12] << 8 | f[13];
	pl = f + 14;
	plen = n - 14;
	if (proto <= 1500) {			/* 802.3 length: LLC/SNAP follows */
		if (n >= 22 && f[14] == 0xaa && f[15] == 0xaa && f[16] == 0x03) {
			proto = f[20] << 8 | f[21];
			pl = f + 22;
			plen = n - 22;
		} else {
			pr->stats.rx_dropped++;
			return;
		}
	}
	skb = dev_alloc_skb(plen + 14 + 2);
	if (!skb) {
		pr->stats.rx_dropped++;
		return;
	}
	skb_reserve(skb, 2);
	e = skb_put(skb, 14);
	memcpy(e, f, 12);
	e[12] = proto >> 8;
	e[13] = proto;
	memcpy(skb_put(skb, plen), pl, plen);
	skb->dev = pw_dev;
	skb->protocol = eth_type_trans(skb, pw_dev);
	pr->stats.rx_packets++;
	pr->stats.rx_bytes += plen + 14;
	pw_dev->last_rx = jiffies;
	netif_rx(skb);
}

/* look at the chip; called with bus_mutex held */
static void pw_poll(void)
{
	u8 s = 0;
	int k;

	if (rd8(0x54, &s))
		return;
	if (s & 0x08) {				/* async event: code in 0x4e, ack with 0xf7 */
		u32 ev = 0;

		rd32raw(0x4e, &ev);
		wr8(0x54, 0xf7);
		pw_events++;
		printk(KERN_INFO "wlan0: chip event %08x\n", ev);
	}
	if (s & 0x10) {				/* unsolicited firmware message */
		u16 rl = 0;

		fw_read_resp(&rl, 1);
		wr8(0x56, 0x10);
		printk(KERN_INFO "wlan0: fw msg cmd=%02x%02x len=%04x\n", rbuf[1], rbuf[0], rl);
	}
	for (k = 0; k < 8; k++) {
		u16 l = 0;
		int al, r;

		if (rd16(0x40, &l) || l < 0x18 || l >= 0x7f0)
			break;
		al = (l + 3) & ~3;
		memset(pw_rxbuf, 0, al);
		ms_dma_mode(0);
		r = blk_read(pw_rxbuf, al, 0x102, 0xb3);
		ms_dma_mode(3);
		if (r) {
			printk(KERN_INFO "wlan0: rx read -> %d\n", r);
			break;
		}
		{
			int hl = pw_rxbuf[8], fl = pw_rxbuf[4] | pw_rxbuf[5] << 8;

			if (hl < 0x10 || hl > 0x40 || hl + fl > l)
				fl = l - 0x10, hl = 0x10;
			pw_rx_frame(pw_rxbuf + hl, fl);
		}
	}
}

static void pw_tx(struct sk_buff *skb)
{
	struct pw_priv *pr = netdev_priv(pw_dev);
	int n = skb->len, al, r;

	if (n < 14 || n > 1514) {
		pr->stats.tx_errors++;
		dev_kfree_skb(skb);
		return;
	}
	al = (24 + n + 3) & ~3;
	memset(pw_txbuf, 0, al);
	pw_txbuf[0] = 1;
	pw_txbuf[5] = 0x40;
	pw_txbuf[6] = 1;
	pw_txbuf[8] = 0x18;
	pw_txbuf[12] = n;
	pw_txbuf[13] = n >> 8;
	memcpy(pw_txbuf + 14, skb->data, 6);	/* destination MAC */
	memcpy(pw_txbuf + 24, skb->data, n);
	ms_dma_mode(1);
	r = wr16(0x42, al);
	blk_wr_addr = 0x102;
	if (!r)
		r = blk_write(pw_txbuf, al);
	blk_wr_addr = 0x100;
	ms_dma_mode(0);
	if (r) {
		pr->stats.tx_errors++;
	} else {
		pr->stats.tx_packets++;
		pr->stats.tx_bytes += n;
	}
	dev_kfree_skb(skb);
}

static int pw_thread(void *arg)
{
	while (!kthread_should_stop()) {
		struct sk_buff *skb;

		wait_event_timeout(pw_wq, !skb_queue_empty(&pw_txq) || kthread_should_stop(),
				   msecs_to_jiffies(10));
		mutex_lock(&bus_mutex);
		pw_poll();
		while ((skb = skb_dequeue(&pw_txq)) != NULL) {
			pw_tx(skb);
			if (skb_queue_len(&pw_txq) < 8 && netif_queue_stopped(pw_dev))
				netif_wake_queue(pw_dev);
		}
		mutex_unlock(&bus_mutex);
		msleep(1);
	}
	return 0;
}

static int pw_open(struct net_device *dev)
{
	netif_start_queue(dev);
	return 0;
}

static int pw_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

static int pw_xmit(struct sk_buff *skb, struct net_device *dev)
{
	skb_queue_tail(&pw_txq, skb);
	dev->trans_start = jiffies;
	if (skb_queue_len(&pw_txq) >= 32)
		netif_stop_queue(dev);
	wake_up(&pw_wq);
	return 0;
}

static struct net_device_stats *pw_stats(struct net_device *dev)
{
	return &((struct pw_priv *)netdev_priv(dev))->stats;
}

/* "netup": register wlan0 and start polling.  Call after join/assoc. */
static void net_up(void)
{
	int r;

	log_reset();
	if (pw_dev) {
		logf("netup: already up\n");
		return;
	}
	pw_dev = alloc_etherdev(sizeof(struct pw_priv));
	if (!pw_dev) {
		logf("netup: no memory\n");
		return;
	}
	strcpy(pw_dev->name, "wlan%d");
	memcpy(pw_dev->dev_addr, psp_mac, 6);
	pw_dev->open = pw_open;
	pw_dev->stop = pw_stop;
	pw_dev->hard_start_xmit = pw_xmit;
	pw_dev->get_stats = pw_stats;
	pw_dev->tx_queue_len = 32;
	skb_queue_head_init(&pw_txq);
	r = register_netdev(pw_dev);
	if (r) {
		logf("netup: register_netdev -> %d\n", r);
		free_netdev(pw_dev);
		pw_dev = NULL;
		return;
	}
	netif_carrier_on(pw_dev);
	pw_task = kthread_run(pw_thread, NULL, "wlan0");
	if (IS_ERR(pw_task)) {
		logf("netup: kthread -> %ld\n", PTR_ERR(pw_task));
		pw_task = NULL;
	}
	logf("netup: %s mac %02x:%02x:%02x:%02x:%02x:%02x\n", pw_dev->name,
	     psp_mac[0], psp_mac[1], psp_mac[2], psp_mac[3], psp_mac[4], psp_mac[5]);
}

static void net_down(void)
{
	if (!pw_dev)
		return;
	if (pw_task) {
		kthread_stop(pw_task);
		pw_task = NULL;
	}
	unregister_netdev(pw_dev);
	skb_queue_purge(&pw_txq);
	free_netdev(pw_dev);
	pw_dev = NULL;
}

static void run_cmd(char *cmd)
{
	unsigned long a, v;
	char *e;
	u32 w = 0;
	u8 b = 0;
	int r;

	if (!strncmp(cmd, "netup", 5)) {
		net_up();
	} else if (!strncmp(cmd, "chipinit", 8)) {
		run_chipinit();
	} else if (!strncmp(cmd, "link ", 5)) {
		run_link(cmd + 5);
	} else if (!strncmp(cmd, "go", 2)) {
		run_probe();
	} else if (!strncmp(cmd, "off", 3)) {
		power_off();
	} else if (!strncmp(cmd, "scan", 4)) {
		run_scan(cmd[4] == ' ' && cmd[5] == '1');
	} else if (!strncmp(cmd, "cmdx ", 5)) {
		/* cmdx <hexid> <hexlen> <hex bytes...>: payload starts at offset 8 */
		static u8 xb[0x200];
		int k, n = 8;
		unsigned long id, ln;

		id = simple_strtoul(cmd + 5, &e, 16);
		while (*e == ' ')
			e++;
		ln = simple_strtoul(e, &e, 16);
		if (ln < 8 || ln > 0x1f0) {
			log_reset();
			logf("cmdx: bad len %lx\n", ln);
			return;
		}
		memset(xb, 0, sizeof(xb));
		while (*e && n < ln && n < 0x1f0) {
			while (*e == ' ' || *e == '\n')
				e++;
			if (!*e)
				break;
			xb[n++] = simple_strtoul(e, &e, 16);
		}
		(void)k;
		log_reset();
		fill_hdr(xb, id, ln);
		{
			u16 rl;

			r = fw_xfer(xb, (ln + 3) & ~3, &rl, 600);
			logf("cmdx %04lx len %04lx -> %d\n", id, ln, r);
			if (!r)
				show_resp(rl, 0x30);
		}
	} else if (!strncmp(cmd, "dhcp", 4)) {
		tx_variant = simple_strtoul(cmd + 4 + (cmd[4] == ' '), &e, 10);
		listen_ms = 15000;
		if (*e == ' ')
			listen_ms = simple_strtoul(e + 1, &e, 10) * 1000;
		run_dhcp();
	} else if (!strncmp(cmd, "rx", 2)) {
		run_rx(simple_strtoul(cmd + 2, &e, 10));
	} else if (!strncmp(cmd, "regs", 4)) {
		log_reset();
		regdump("regs");
	} else if (!strncmp(cmd, "join ", 5)) {
		a = simple_strtoul(cmd + 5, &e, 10);
		while (*e == ' ')
			e++;
		run_join(a, simple_strtoul(e, &e, 16));
	} else if (!strncmp(cmd, "auth ", 5)) {
		a = simple_strtoul(cmd + 5, &e, 10);
		while (*e == ' ')
			e++;
		run_auth(a, simple_strtoul(e, &e, 10));
	} else if (!strncmp(cmd, "assoc ", 6)) {
		unsigned long f[4];
		int k;

		a = simple_strtoul(cmd + 6, &e, 10);
		for (k = 0; k < 4; k++) {
			while (*e == ' ')
				e++;
			f[k] = simple_strtoul(e, &e, 16);
		}
		run_assoc(a, f[0], f[1], f[2], f[3]);
	} else if (!strncmp(cmd, "cmd ", 4)) {
		a = simple_strtoul(cmd + 4, &e, 16);
		while (*e == ' ')
			e++;
		v = simple_strtoul(e, &e, 16);
		run_fwcmd(a, v);
	} else if (!strncmp(cmd, "swap", 4)) {
		log_reset();
		wr_swap = (cmd[4] == ' ' && cmd[5] == '1');
		logf("swap=%d\n", wr_swap);
	} else if (!strncmp(cmd, "info", 4)) {
		run_info();
	} else if (!strncmp(cmd, "init", 4)) {
		hal_init();
	} else if (!strncmp(cmd, "exp", 3)) {
		run_exp();
	} else if (!strncmp(cmd, "fw", 2)) {
		run_fw();
	} else if (!strncmp(cmd, "gi", 2)) {
		log_reset();
		r = ms_get_int(&w);
		logf("gi -> %d, int=%02x\n", r, w & 0xff);
	} else if (!strncmp(cmd, "rd ", 3)) {
		a = simple_strtoul(cmd + 3, &e, 16);
		log_reset();
		r = rd8(a, &b);
		logf("rd %02lx -> %d, val=%02x\n", a, r, b);
	} else if (!strncmp(cmd, "wr ", 3)) {
		a = simple_strtoul(cmd + 3, &e, 16);
		while (*e == ' ')
			e++;
		v = simple_strtoul(e, &e, 16);
		log_reset();
		r = wr8(a, v);
		logf("wr %02lx=%02lx -> %d\n", a, v, r);
	}
}

static int wlan_read(char *page, char **start, off_t off, int count,
		     int *eof, void *data)
{
	int n = loglen - off;

	if (n <= 0) {
		*eof = 1;
		return 0;
	}
	if (n > count)
		n = count;
	else
		*eof = 1;
	memcpy(page, logbuf + off, n);
	*start = page;
	return n;
}

static int wlan_write(struct file *file, const char __user *buf,
		      unsigned long count, void *data)
{
	static char cmd[512];
	unsigned long n = count < sizeof(cmd) - 1 ? count : sizeof(cmd) - 1;

	if (copy_from_user(cmd, buf, n))
		return -EFAULT;
	cmd[n] = 0;
	if (!strncmp(cmd, "netdown", 7)) {
		net_down();			/* joins the poll thread, so no bus_mutex here */
		return count;
	}
	mutex_lock(&bus_mutex);
	run_cmd(cmd);
	mutex_unlock(&bus_mutex);
	return count;
}

static int fw_write(struct file *file, const char __user *buf,
		    unsigned long count, void *data)
{
	int is_main = data != NULL;
	u8 **img = is_main ? &fwm : &fwh;
	int *len = is_main ? &fwm_len : &fwh_len;
	int max = is_main ? MAIN_MAX : HELPER_MAX;

	if (count == 4) {
		char c[4];

		if (copy_from_user(c, buf, 4))
			return -EFAULT;
		if (!memcmp(c, "clr\n", 4)) {
			*len = 0;
			return count;
		}
	}
	if (!*img) {
		*img = kmalloc(max, GFP_KERNEL);
		if (!*img)
			return -ENOMEM;
	}
	if (*len + count > max)
		return -ENOSPC;
	if (copy_from_user(*img + *len, buf, count))
		return -EFAULT;
	*len += count;
	return count;
}

static int fw_read(char *page, char **start, off_t off, int count,
		   int *eof, void *data)
{
	*eof = 1;
	return sprintf(page, "%d bytes loaded\n", *(data ? &fwm_len : &fwh_len));
}

static int __init psp_wlan_probe_init(void)
{
	struct proc_dir_entry *e;

	e = create_proc_entry("psp_wlan", 0644, NULL);
	if (!e)
		return -ENOMEM;
	e->read_proc = wlan_read;
	e->write_proc = wlan_write;
	e = create_proc_entry("psp_fwh", 0644, NULL);
	if (e) {
		e->data = NULL;
		e->read_proc = fw_read;
		e->write_proc = fw_write;
	}
	e = create_proc_entry("psp_fwm", 0644, NULL);
	if (e) {
		e->data = (void *)1;
		e->read_proc = fw_read;
		e->write_proc = fw_write;
	}
	logf("ready: go | fw | rd A | wr A V | gi | off\n");
	return 0;
}
device_initcall(psp_wlan_probe_init);

MODULE_AUTHOR("Anastasiia Velikaya");
MODULE_DESCRIPTION("PSP WLAN bring-up probe");
MODULE_LICENSE("GPL");
