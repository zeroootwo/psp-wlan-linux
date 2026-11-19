/*
 * autoboot: VSH plugin for PRO-family custom firmware that starts an EBOOT (the uClinux loader) right after power-on,
 * so the XMB never has to be used.
 *
 *  - waits ~10 s for the XMB to come up; if L is held at any time during that wait, it does nothing (escape to the XMB)
 *  - only acts on a fresh power-on (system time < 90 s): when the XMB comes back after the program exited or crashed
 *    it stays quiet, so there is no restart loop
 *  - the EBOOT to start is the first line of ms0:/seplugins/autoboot.txt (default below); if ms0:/seplugins/autoboot-once.txt
 *    exists, its first line is used instead and the file is deleted first (one-shot test boots)
 *  - progress is appended to ms0:/seplugins/autoboot.log
 *
 * Install: copy autoboot.prx to ms0:/seplugins/ and add the line  ms0:/seplugins/autoboot.prx 1  to ms0:/seplugins/vsh.txt
 * Copyright (C) 2026 Anastasiia Velikaya. GPL-2.0.
 */
#include <pspkernel.h>
#include <pspctrl.h>
#include <psploadexec_kernel.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdio.h>

PSP_MODULE_INFO("autoboot", 0x1007, 1, 0);

#define CFG "ms0:/seplugins/autoboot.txt"
#define ONCE "ms0:/seplugins/autoboot-once.txt"
#define DEF "ms0:/PSP/GAME/uClinux_exp/EBOOT.PBP"
#define LOG "ms0:/seplugins/autoboot.log"
#define WAIT_STEPS 200			/* x 50 ms = 10 s */

static void note(const char *fmt, int a)
{
	char b[160];
	int n = snprintf(b, sizeof(b), fmt, a);
	SceUID f = sceIoOpen(LOG, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);

	if (f >= 0) {
		sceIoWrite(f, b, n);
		sceIoClose(f);
	}
}

static int read_line(const char *file, char *out, int outsz)
{
	char b[200];
	int n, k = 0;
	SceUID f = sceIoOpen(file, PSP_O_RDONLY, 0);

	if (f < 0)
		return 0;
	n = sceIoRead(f, b, sizeof(b) - 1);
	sceIoClose(f);
	if (n <= 0)
		return 0;
	while (k < n && b[k] != '\r' && b[k] != '\n')
		k++;
	if (k <= 0 || k >= outsz)
		return 0;
	memcpy(out, b, k);
	out[k] = 0;
	return 1;
}

static int worker(SceSize args, void *argp)
{
	SceCtrlData pad;
	char path[200];
	SceIoStat st;
	struct SceKernelLoadExecVSHParam param;
	unsigned int up = sceKernelGetSystemTimeLow() / 1000000;
	int i, rc;

	note("autoboot: started at uptime %d s\n", (int)up);
	if (up > 90) {
		note("autoboot: not a fresh power-on, staying quiet\n", 0);
		return 0;
	}
	for (i = 0; i < WAIT_STEPS; i++) {
		sceCtrlPeekBufferPositive(&pad, 1);
		if (pad.Buttons & PSP_CTRL_LTRIGGER) {
			note("autoboot: L held, aborted after %d steps\n", i);
			return 0;
		}
		sceKernelDelayThread(50 * 1000);
	}
	strcpy(path, DEF);
	if (!read_line(CFG, path, sizeof(path)))
		strcpy(path, DEF);
	if (read_line(ONCE, path, sizeof(path))) {
		sceIoRemove(ONCE);			/* one shot: gone before we start anything */
		note("autoboot: one-shot target used\n", 0);
	}
	if (sceIoGetstat(path, &st) < 0) {
		note("autoboot: target not found\n", 0);
		return 0;
	}
	note("autoboot: launching\n", 0);
	memset(&param, 0, sizeof(param));
	param.size = sizeof(param);
	param.args = strlen(path) + 1;
	param.argp = path;
	param.key = "game";
	rc = sceKernelLoadExecVSHMs2(path, &param);
	note("autoboot: LoadExec returned %08x\n", rc);
	return 0;
}

int module_start(SceSize args, void *argp)
{
	SceUID th = sceKernelCreateThread("autoboot", worker, 0x30, 0x4000, 0, NULL);

	if (th >= 0)
		sceKernelStartThread(th, 0, NULL);
	return 0;
}

int module_stop(SceSize args, void *argp) { return 0; }
