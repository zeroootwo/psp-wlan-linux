/*
 * wlanlog: PRO-CFW kernel plugin that logs the host<->chip block traffic of
 * Sony's sceWlan_Driver (6.61, PSP-3000) to ms0:/wlanlog.bin.
 *
 * Hooks (offsets in wlan.prx text, from the decompilation):
 *   0xc6a8  block write  (host, addr, len, buf)   0xb4  -> every TX to the chip
 *   0xbe74  block read   (host, addr, len, buf)   0xb2
 *   0xc0f0  block read   (host, addr, len, buf)   0xb3
 *   0xc3cc  block read   (host, addr, len, buf)   0xb3 (same buf each chunk)
 * Record format (little endian):
 *   u32 time_us, u8 kind, u8 flags, u16 len, u32 addr, u16 logged, u16 rsvd,
 *   then `logged` bytes padded to 4.
 *   kind: 1 = write, 2 = read b2, 3 = read b3 (c0f0), 4 = read b3 (c3cc)
 *   register helpers (addr = chip reg, data = value bytes):
 *   5 = rd8 (0x97a0), 6 = wr8 (0x9818), 7 = rd16 (0xb120), 8 = wr16 (0xb1a0),
 *   9 = rd32 (0xb21c), 10 = dma mode (0x8f80, addr = mode)
 */
#include <pspkernel.h>
#include <pspsdk.h>
#include <psploadcore.h>
#include <pspintrman.h>
#include <psputils.h>
#include <string.h>
#include <stdio.h>

PSP_MODULE_INFO("wlanlog", 0x1007, 1, 0);

#define RING_SZ   (512 * 1024)
#define CAP_BIG   64
#define CAP_MAX   0x600

static u8 ring[RING_SZ] __attribute__((aligned(64)));
static volatile u32 rhead, rtail, dropped;

struct rec { u32 t; u8 kind; u8 flags; u16 len; u32 addr; u16 logged; u16 rsvd; };

static void log_rec(int kind, u32 addr, u32 len, const void *buf)
{
	u32 cap = len;
	if (cap > 0x400) cap = CAP_BIG;
	if (cap > CAP_MAX) cap = CAP_MAX;
	if (!buf || (u32)buf < 0x08000000) cap = 0;
	u32 total = sizeof(struct rec) + ((cap + 3) & ~3u);

	u32 intr = sceKernelCpuSuspendIntr();
	u32 h = rhead, t = rtail;
	u32 used = (h - t) % RING_SZ;
	if (RING_SZ - used - 1 < total) {
		dropped++;
		sceKernelCpuResumeIntr(intr);
		return;
	}
	struct rec r;
	r.t = sceKernelGetSystemTimeLow();
	r.kind = kind; r.flags = 0;
	r.len = len; r.addr = addr; r.logged = cap; r.rsvd = (u16)dropped;
	const u8 *s = (const u8 *)&r;
	u32 i;
	for (i = 0; i < sizeof(r); i++) { ring[h] = s[i]; h = (h + 1) % RING_SZ; }
	const u8 *d = (const u8 *)buf;
	for (i = 0; i < cap; i++) { ring[h] = d[i]; h = (h + 1) % RING_SZ; }
	for (; i % 4; i++) { ring[h] = 0; h = (h + 1) % RING_SZ; }
	rhead = h;
	sceKernelCpuResumeIntr(intr);
}

/* ---- trampolines: [orig0, orig1, j target+8, nop] ---- */
#define NHOOK 10
static u32 tramp[NHOOK][4] __attribute__((aligned(64)));

typedef int (*blk_fn)(int host, u32 addr, u32 len, void *buf);

static int h_write(int host, u32 addr, u32 len, void *buf)
{
	log_rec(1, addr, len, buf);
	return ((blk_fn)tramp[0])(host, addr, len, buf);
}
static int h_rb2(int host, u32 addr, u32 len, void *buf)
{
	int r = ((blk_fn)tramp[1])(host, addr, len, buf);
	log_rec(2, addr, len, buf);
	return r;
}
static int h_rb3a(int host, u32 addr, u32 len, void *buf)
{
	int r = ((blk_fn)tramp[2])(host, addr, len, buf);
	log_rec(3, addr, len, buf);
	return r;
}
static int h_rb3b(int host, u32 addr, u32 len, void *buf)
{
	int r = ((blk_fn)tramp[3])(host, addr, len, buf);
	log_rec(4, addr, len, buf);
	return r;
}

typedef int (*reg_fn)(int host, u32 reg, void *p);
static int h_rd8(int host, u32 reg, void *p)  { int r = ((reg_fn)tramp[4])(host, reg, p); log_rec(5, reg, 1, p); return r; }
static int h_wr8(int host, u32 reg, void *p)  { log_rec(6, reg, 1, p); return ((reg_fn)tramp[5])(host, reg, p); }
static int h_rd16(int host, u32 reg, void *p) { int r = ((reg_fn)tramp[6])(host, reg, p); log_rec(7, reg, 2, p); return r; }
static int h_wr16(int host, u32 reg, void *p) { log_rec(8, reg, 2, p); return ((reg_fn)tramp[7])(host, reg, p); }
static int h_rd32(int host, u32 reg, void *p) { int r = ((reg_fn)tramp[8])(host, reg, p); log_rec(9, reg, 4, p); return r; }
static int h_dma(int host, u32 mode)          { log_rec(10, mode, 0, NULL); return ((int (*)(int, u32))tramp[9])(host, mode); }

struct target { u32 off, w0, w1; void *hook; };
static struct target tg[NHOOK] = {
	{ 0xc6a8, 0x27bdff90, 0xafb60058, h_write },
	{ 0xbe74, 0x27bdff90, 0xafb7005c, h_rb2   },
	{ 0xc0f0, 0x27bdff90, 0xafb60058, h_rb3a  },
	{ 0xc3cc, 0x27bdff90, 0xafb60058, h_rb3b  },
	{ 0x97a0, 0x27bdfff0, 0xafbf000c, h_rd8   },
	{ 0x9818, 0x27bdfff0, 0xafbf000c, h_wr8   },
	{ 0xb120, 0x27bdfff0, 0xafbf000c, h_rd16  },
	{ 0xb1a0, 0x27bdfff0, 0xafbf000c, h_wr16  },
	{ 0xb21c, 0x27bdfff0, 0xafbf000c, h_rd32  },
	{ 0x8f80, 0x27bdfff0, 0xafb10004, h_dma   },
};

#define J(a) (0x08000000u | ((((u32)(a)) & 0x0fffffffu) >> 2))

static char status[1024];
static int installed;

static int install(void)
{
	SceModule *m = sceKernelFindModuleByName("sceWlan_Driver");
	if (!m) return 0;
	u32 base = m->text_addr;
	int i, ok = 1;
	int n = snprintf(status, sizeof(status), "text_addr=%08x text_size=%08x\n", (unsigned)base, (unsigned)m->text_size);
	for (i = 0; i < NHOOK; i++) {
		volatile u32 *p = (volatile u32 *)(base + tg[i].off);
		n += snprintf(status + n, sizeof(status) - n, "%04x: %08x %08x%s\n", (unsigned)tg[i].off, (unsigned)p[0], (unsigned)p[1],
			(p[0] == tg[i].w0 && p[1] == tg[i].w1) ? "" : " MISMATCH");
		if (p[0] != tg[i].w0 || p[1] != tg[i].w1) ok = 0;
	}
	if (!ok) { n += snprintf(status + n, sizeof(status) - n, "NOT PATCHED\n"); installed = -1; return 1; }

	u32 intr = sceKernelCpuSuspendIntr();
	for (i = 0; i < NHOOK; i++) {
		u32 *p = (u32 *)(base + tg[i].off);
		tramp[i][0] = p[0]; tramp[i][1] = p[1];
		tramp[i][2] = J(p + 2); tramp[i][3] = 0;
	}
	sceKernelDcacheWritebackInvalidateRange(tramp, sizeof(tramp));
	sceKernelIcacheInvalidateRange(tramp, sizeof(tramp));
	for (i = 0; i < NHOOK; i++) {
		u32 *p = (u32 *)(base + tg[i].off);
		p[0] = J(tg[i].hook); p[1] = 0;
		sceKernelDcacheWritebackInvalidateRange(p, 8);
		sceKernelIcacheInvalidateRange(p, 8);
	}
	sceKernelCpuResumeIntr(intr);
	snprintf(status + n, sizeof(status) - n, "PATCHED\n");
	installed = 1;
	return 1;
}

static void write_status(void)
{
	SceUID f = sceIoOpen("ms0:/wlanlog.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (f < 0) return;
	sceIoWrite(f, status, strlen(status));
	sceIoClose(f);
}

static int flush(int *fd_ok)
{
	u32 h = rhead, t = rtail;
	if (h == t) return 0;
	SceUID f = sceIoOpen("ms0:/wlanlog.bin", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if (f < 0) { *fd_ok = 0; return 0; }
	*fd_ok = 1;
	if (h > t) sceIoWrite(f, ring + t, h - t);
	else { sceIoWrite(f, ring + t, RING_SZ - t); if (h) sceIoWrite(f, ring, h); }
	sceIoClose(f);
	rtail = h;
	return 1;
}

static int worker(SceSize args, void *argp)
{
	int fd_ok = 0, truncated = 0, status_done = 0, waited = 0;
	for (;;) {
		if (!installed && waited < 4800) { install(); waited++; }
		if (installed && !status_done) {
			SceUID f = sceIoOpen("ms0:/wlanlog.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
			if (f >= 0) { sceIoClose(f); write_status(); status_done = 1; }
		}
		if (!truncated) {
			SceUID f = sceIoOpen("ms0:/wlanlog.bin", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
			if (f >= 0) { sceIoClose(f); truncated = 1; }
		} else {
			flush(&fd_ok);
		}
		sceKernelDelayThread(250 * 1000);
	}
	return 0;
}

int module_start(SceSize args, void *argp)
{
	SceUID th = sceKernelCreateThread("wlanlog", worker, 0x30, 0x4000, 0, NULL);
	if (th >= 0) sceKernelStartThread(th, 0, NULL);
	return 0;
}

int module_stop(SceSize args, void *argp) { return 0; }
