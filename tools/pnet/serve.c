/*
 * pnet serve: remote access for the PSP (no fork, no floating point).
 *
 * One TCP port.  After a challenge-response login (HMAC-SHA1 over a random nonce with a shared key) the client
 * sends one command line:
 *   SHELL                  bridge the connection to a persistent shell (see below)
 *   RUN <timeout> <cmd>    run one command line in that shell, get its output and exit code back
 *   GET <path>             download a file
 *   PUT <size> <mode> <path>   upload a file (written to <path>.tmp, then renamed)
 *   PING                   liveness check
 *
 * The shell is an ordinary `sh` started by the boot script with its stdin/stdout on two FIFOs that this
 * server holds open (the rootfs has no fork for us, the shell's own job control spawns the programs).
 * Only hosts of the interface's subnet are accepted.  Traffic is not encrypted: use it on a network you trust.
 *
 * Copyright (C) 2026 Anastasiia Velikaya. GPL-2.0.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "wpa.h"

#define MAXOUT (96 * 1024)
#define MARK "@@END@@"

static u8 key[64];
static int keylen;
static int shin = -1, shout = -1;
static const char *g_if = "wlan0";
static int g_any;			/* test mode: accept any peer */

static int wr_all(int fd, const void *b, int n)
{
	const char *p = b;

	while (n > 0) {
		int k = write(fd, p, n);

		if (k < 0) {
			if (errno == EINTR || errno == EAGAIN) {
				usleep(1000);
				continue;
			}
			return -1;
		}
		p += k;
		n -= k;
	}
	return 0;
}

static int rd_exact(int fd, void *b, int n)
{
	char *p = b;

	while (n > 0) {
		int k = read(fd, p, n);

		if (k <= 0)
			return -1;
		p += k;
		n -= k;
	}
	return 0;
}

/* one line, up to n-1 bytes, no echo; returns length or -1 */
static int rd_line(int fd, char *b, int n)
{
	int i = 0;

	while (i < n - 1) {
		char c;
		int k = read(fd, &c, 1);

		if (k <= 0)
			return -1;
		if (c == '\n')
			break;
		if (c != '\r')
			b[i++] = c;
	}
	b[i] = 0;
	return i;
}

static void say(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(int fd, const char *fmt, ...)
{
	char b[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (n > 0)
		wr_all(fd, b, n < (int)sizeof(b) ? n : (int)sizeof(b) - 1);
}

static void make_nonce(u8 *n16)
{
	static unsigned long ctr;
	u8 seed[48], h[20];
	struct timeval tv;
	int fd = open("/dev/urandom", O_RDONLY);

	if (fd >= 0) {
		int ok = read(fd, n16, 16) == 16;

		close(fd);
		if (ok)
			return;
	}
	memset(seed, 0, sizeof(seed));
	gettimeofday(&tv, NULL);
	memcpy(seed, &tv, sizeof(tv) < 16 ? sizeof(tv) : 16);
	ctr++;
	memcpy(seed + 16, &ctr, sizeof(ctr));
	{
		pid_t p = getpid();

		memcpy(seed + 24, &p, sizeof(p));
	}
	hmac_sha1(key, keylen, seed, sizeof(seed), h);
	memcpy(n16, h, 16);
}

/* is the peer inside the subnet of the interface that holds the address it connected to? */
static int peer_ok(int c)
{
	struct sockaddr_in me, pe;
	socklen_t l = sizeof(me), l2 = sizeof(pe);
	struct ifreq ifr;
	int s;
	uint32_t mask;

	if (g_any)
		return 1;
	if (getsockname(c, (struct sockaddr *)&me, &l) < 0 || getpeername(c, (struct sockaddr *)&pe, &l2) < 0)
		return 0;
	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return 0;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, g_if, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIFNETMASK, &ifr) < 0) {
		close(s);
		return 0;
	}
	close(s);
	mask = ((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr.s_addr;
	return (me.sin_addr.s_addr & mask) == (pe.sin_addr.s_addr & mask);
}

static int login(int c)
{
	u8 nonce[16], want[20], got[20];

	make_nonce(nonce);
	if (wr_all(c, "PSP1 ", 5) || wr_all(c, nonce, 16))
		return -1;
	if (rd_exact(c, got, 20))
		return -1;
	hmac_sha1(key, keylen, nonce, 16, want);
	if (memcmp(got, want, 20)) {
		say(c, "NO\n");
		sleep(1);		/* slow down guessing */
		return -1;
	}
	say(c, "OK\n");
	return 0;
}

/* ---- commands ---- */

static void drain_shell(void)
{
	char b[512];

	while (read(shout, b, sizeof(b)) > 0)
		;
}

static void do_run(int c, int tmo, const char *cmd)
{
	static char out[MAXOUT + 64];
	int n = 0, code = -1, trunc = 0;
	time_t end = time(NULL) + (tmo > 0 ? tmo : 30);
	char *m = NULL;

	if (shin < 0 || shout < 0) {
		say(c, "ERR no shell\n");
		return;
	}
	drain_shell();
	wr_all(shin, cmd, strlen(cmd));
	wr_all(shin, "\n", 1);
	{
		static const char endcmd[] = "echo " MARK "$?\n";

		wr_all(shin, endcmd, sizeof(endcmd) - 1);
	}
	for (;;) {
		fd_set rf;
		struct timeval tv = { 0, 200000 };
		int k;

		out[n] = 0;
		m = strstr(out, MARK);
		if (m && strchr(m, '\n'))
			break;
		if (time(NULL) >= end) {
			code = -2;
			break;
		}
		FD_ZERO(&rf);
		FD_SET(shout, &rf);
		if (select(shout + 1, &rf, NULL, NULL, &tv) <= 0)
			continue;
		k = read(shout, out + n, MAXOUT - n);
		if (k > 0) {
			n += k;
			if (n >= MAXOUT) {
				trunc = 1;
				n = MAXOUT - 1;
			}
		}
	}
	if (m && code != -2) {
		code = atoi(m + 7);
		n = m - out;
	}
	say(c, "OK %d %d %d\n", n, code, trunc);
	wr_all(c, out, n);
}

static void do_get(int c, const char *path)
{
	char b[1024];
	struct stat st;
	int fd = open(path, O_RDONLY), k;

	if (fd < 0 || fstat(fd, &st) < 0) {
		say(c, "ERR %s\n", strerror(errno));
		if (fd >= 0)
			close(fd);
		return;
	}
	say(c, "OK %ld\n", (long)st.st_size);
	while ((k = read(fd, b, sizeof(b))) > 0)
		if (wr_all(c, b, k))
			break;
	close(fd);
}

static void do_put(int c, long size, int mode, const char *path)
{
	char tmp[300], b[1024];
	int fd;
	long left = size;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode ? mode : 0644);
	if (fd < 0) {
		say(c, "ERR %s\n", strerror(errno));
		return;
	}
	say(c, "GO\n");
	while (left > 0) {
		int k = read(c, b, left < (long)sizeof(b) ? (int)left : (int)sizeof(b));

		if (k <= 0) {
			close(fd);
			unlink(tmp);
			return;
		}
		if (write(fd, b, k) != k) {
			close(fd);
			unlink(tmp);
			say(c, "ERR write: %s\n", strerror(errno));
			return;
		}
		left -= k;
	}
	close(fd);
	if (rename(tmp, path) < 0) {
		say(c, "ERR rename: %s\n", strerror(errno));
		return;
	}
	say(c, "OK %ld\n", size);
}

static void do_shell(int c)
{
	char b[1024];

	if (shin < 0 || shout < 0) {
		say(c, "ERR no shell\n");
		return;
	}
	say(c, "OK\n");
	drain_shell();
	for (;;) {
		fd_set rf;
		int k;

		FD_ZERO(&rf);
		FD_SET(c, &rf);
		FD_SET(shout, &rf);
		if (select((c > shout ? c : shout) + 1, &rf, NULL, NULL, NULL) < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		if (FD_ISSET(c, &rf)) {
			k = read(c, b, sizeof(b));
			if (k <= 0)
				return;
			wr_all(shin, b, k);
		}
		if (FD_ISSET(shout, &rf)) {
			k = read(shout, b, sizeof(b));
			if (k > 0 && wr_all(c, b, k))
				return;
		}
	}
}

static void client(int c)
{
	char line[600];
	struct timeval tmo = { 60, 0 };

	setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
	setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof(tmo));
	if (!peer_ok(c)) {
		say(c, "NO\n");
		return;
	}
	if (login(c))
		return;
	if (rd_line(c, line, sizeof(line)) < 0)
		return;
	if (!strcmp(line, "SHELL")) {
		struct timeval none = { 0, 0 };

		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
		do_shell(c);
	} else if (!strncmp(line, "RUN ", 4)) {
		char *p = line + 4;
		int t = atoi(p);

		while (*p && *p != ' ')
			p++;
		while (*p == ' ')
			p++;
		do_run(c, t, p);
	} else if (!strncmp(line, "GET ", 4)) {
		do_get(c, line + 4);
	} else if (!strncmp(line, "PUT ", 4)) {
		long size = strtol(line + 4, NULL, 10);
		char *p = strchr(line + 4, ' ');
		int mode;

		if (!p) {
			say(c, "ERR syntax\n");
			return;
		}
		mode = (int)strtol(p + 1, &p, 8);
		while (*p == ' ')
			p++;
		do_put(c, size, mode, p);
	} else if (!strcmp(line, "PING")) {
		say(c, "PONG\n");
	} else {
		say(c, "ERR unknown command\n");
	}
}

static int load_key(const char *path)
{
	FILE *f = fopen(path, "r");
	char hex[200];
	int n = 0, i;

	if (!f) {
		perror(path);
		return -1;
	}
	if (!fgets(hex, sizeof(hex), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	for (i = 0; hex[i] && hex[i + 1] && n < (int)sizeof(key); i += 2) {
		int a = hex[i], b = hex[i + 1];

		a = a >= '0' && a <= '9' ? a - '0' : a >= 'a' && a <= 'f' ? a - 'a' + 10 : a >= 'A' && a <= 'F' ? a - 'A' + 10 : -1;
		b = b >= '0' && b <= '9' ? b - '0' : b >= 'a' && b <= 'f' ? b - 'a' + 10 : b >= 'A' && b <= 'F' ? b - 'A' + 10 : -1;
		if (a < 0 || b < 0)
			break;
		key[n++] = a << 4 | b;
	}
	keylen = n;
	return n >= 16 ? 0 : -1;
}

/* pnet serve <port> <keyfile> [fifo_in fifo_out [ifname|any [bind_addr]]] */
int do_serve(int argc, char **argv)
{
	int port = atoi(argv[0]), ls, one = 1;
	struct sockaddr_in a;
	const char *fin = argc > 2 ? argv[2] : "/dev/psh_in";
	const char *fout = argc > 3 ? argv[3] : "/dev/psh_out";

	if (argc > 4) {
		if (!strcmp(argv[4], "any"))
			g_any = 1;
		else
			g_if = argv[4];
	}
	if (load_key(argv[1])) {
		fprintf(stderr, "serve: bad key file (need >= 32 hex digits)\n");
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);
	shin = open(fin, O_RDWR);
	shout = open(fout, O_RDWR | O_NONBLOCK);
	if (shin < 0 || shout < 0)
		fprintf(stderr, "serve: no shell fifos (%s, %s): only GET/PUT/PING will work\n", fin, fout);
	ls = socket(AF_INET, SOCK_STREAM, 0);
	if (ls < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	if (argc > 5)				/* optional bind address (the host-side test uses 127.0.0.1) */
		a.sin_addr.s_addr = inet_addr(argv[5]);
	if (bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 2) < 0) {
		perror("bind/listen");
		return 1;
	}
	printf("serve: listening on port %d\n", port);
	fflush(stdout);
	for (;;) {
		int c = accept(ls, NULL, NULL);

		if (c < 0) {
			if (errno != EINTR)
				sleep(1);
			continue;
		}
		client(c);
		close(c);
	}
}
