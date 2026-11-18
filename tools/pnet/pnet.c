/*
 * pnet (part of pspwave): tiny network helper for the PSP uClinux rootfs (no fork, no busybox applets needed).
 *   pnet up <if>
 *   pnet addr <if> <ip> <mask>
 *   pnet gw <ip>
 *   pnet dhcp <if>            full DHCP exchange, then configures addr/mask/gw and /etc/resolv.conf
 *   pnet ping <ip> [count]
 *   pnet get <host> [path] [port]    plain HTTP GET, prints the first ~3 KB
 *
 * Copyright (C) 2026 Anastasiia Velikaya. GPL-2.0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include "wpa.h"

#ifndef ETH_P_PAE
#define ETH_P_PAE 0x888e
#endif

static int ctl_sock(void)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);

	if (s < 0)
		perror("socket");
	return s;
}

static int if_up(const char *name)
{
	struct ifreq ifr;
	int s = ctl_sock();

	if (s < 0)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) {
		perror("SIOCGIFFLAGS");
		close(s);
		return -1;
	}
	ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
	if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) {
		perror("SIOCSIFFLAGS");
		close(s);
		return -1;
	}
	close(s);
	return 0;
}

static void sin_set(struct sockaddr *sa, uint32_t ip_be)
{
	struct sockaddr_in *in = (struct sockaddr_in *)sa;

	memset(in, 0, sizeof(*in));
	in->sin_family = AF_INET;
	in->sin_addr.s_addr = ip_be;
}

static int if_addr(const char *name, uint32_t ip, uint32_t mask)
{
	struct ifreq ifr;
	int s = ctl_sock();

	if (s < 0)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	sin_set(&ifr.ifr_addr, ip);
	if (ioctl(s, SIOCSIFADDR, &ifr) < 0) {
		perror("SIOCSIFADDR");
		close(s);
		return -1;
	}
	if (!ip) {			/* 0.0.0.0 = just drop the address */
		close(s);
		return 0;
	}
	sin_set(&ifr.ifr_netmask, mask);
	if (ioctl(s, SIOCSIFNETMASK, &ifr) < 0) {
		perror("SIOCSIFNETMASK");
		close(s);
		return -1;
	}
	close(s);
	return 0;
}

static int add_gw(uint32_t gw)
{
	struct rtentry rt;
	int s = ctl_sock();

	if (s < 0)
		return -1;
	memset(&rt, 0, sizeof(rt));
	sin_set(&rt.rt_dst, 0);
	sin_set(&rt.rt_genmask, 0);
	sin_set(&rt.rt_gateway, gw);
	rt.rt_flags = RTF_UP | RTF_GATEWAY;
	if (ioctl(s, SIOCADDRT, &rt) < 0 && errno != EEXIST) {
		perror("SIOCADDRT");
		close(s);
		return -1;
	}
	close(s);
	return 0;
}

/* ---------------- DHCP ---------------- */

struct dhcp_pkt {
	uint8_t op, htype, hlen, hops;
	uint32_t xid;
	uint16_t secs, flags;
	uint32_t ciaddr, yiaddr, siaddr, giaddr;
	uint8_t chaddr[16];
	uint8_t sname[64];
	uint8_t file[128];
	uint32_t cookie;
	uint8_t opts[308];
} __attribute__((packed));

struct lease {
	uint32_t ip, mask, gw, dns1, dns2, server, secs;
};

static uint16_t ip_csum(const void *p, int n)
{
	const uint8_t *b = p;
	uint32_t s = 0;
	int i;

	for (i = 0; i + 1 < n; i += 2)
		s += b[i] << 8 | b[i + 1];
	if (n & 1)
		s += b[n - 1] << 8;
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return htons(~s);
}

static int dhcp_send(int fd, int ifindex, const uint8_t *mac, uint32_t xid, int type,
		     uint32_t req_ip, uint32_t server)
{
	static uint8_t buf[20 + 8 + sizeof(struct dhcp_pkt)];
	struct dhcp_pkt *d = (struct dhcp_pkt *)(buf + 28);
	uint8_t *ip = buf, *udp = buf + 20, *o = d->opts;
	struct sockaddr_ll to;
	int dl, total;

	memset(buf, 0, sizeof(buf));
	d->op = 1; d->htype = 1; d->hlen = 6;
	d->xid = xid;
	memcpy(d->chaddr, mac, 6);
	d->cookie = htonl(0x63825363);
	*o++ = 53; *o++ = 1; *o++ = type;
	if (type == 3) {
		*o++ = 50; *o++ = 4; memcpy(o, &req_ip, 4); o += 4;
		*o++ = 54; *o++ = 4; memcpy(o, &server, 4); o += 4;
	}
	*o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 15;
	*o++ = 255;
	dl = (uint8_t *)o - (uint8_t *)d;
	if (dl < 300)
		dl = 300;
	total = 28 + dl;
	ip[0] = 0x45; ip[2] = total >> 8; ip[3] = total; ip[8] = 64; ip[9] = 17;
	memset(ip + 16, 0xff, 4);				/* 0.0.0.0 -> 255.255.255.255 */
	{
		uint16_t c = ip_csum(ip, 20);

		memcpy(ip + 10, &c, 2);
	}
	udp[1] = 68; udp[3] = 67; udp[4] = (28 + dl - 20) >> 8; udp[5] = (28 + dl - 20) & 0xff;
	memset(&to, 0, sizeof(to));
	to.sll_family = AF_PACKET;
	to.sll_protocol = htons(ETH_P_IP);
	to.sll_ifindex = ifindex;
	to.sll_halen = 6;
	memset(to.sll_addr, 0xff, 6);
	return sendto(fd, buf, total, 0, (struct sockaddr *)&to, sizeof(to));
}

/* wait for a BOOTREPLY of the given xid; returns DHCP message type or -1 */
static int dhcp_recv(int fd, uint32_t xid, struct lease *l, int tmo_s)
{
	static uint8_t buf[1600];
	time_t end = time(NULL) + tmo_s;

	for (;;) {
		fd_set rf;
		struct timeval tv;
		int n, ihl, type = -1;
		struct dhcp_pkt *d;
		uint8_t *o, *e;

		tv.tv_sec = 1;
		tv.tv_usec = 0;
		FD_ZERO(&rf);
		FD_SET(fd, &rf);
		if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0) {
			if (time(NULL) >= end)
				return -1;
			continue;
		}
		n = recv(fd, buf, sizeof(buf), 0);
		if (n < 20 + 8 + 240)
			continue;
		ihl = (buf[0] & 15) * 4;
		if (buf[9] != 17 || buf[ihl + 2] != 0 || buf[ihl + 3] != 68)
			continue;
		d = (struct dhcp_pkt *)(buf + ihl + 8);
		if (d->op != 2 || d->xid != xid || d->cookie != htonl(0x63825363))
			continue;
		memset(l, 0, sizeof(*l));
		l->ip = d->yiaddr;
		o = d->opts;
		e = buf + n;
		while (o < e && *o != 255) {
			uint8_t c = *o++, ln;

			if (c == 0)
				continue;
			if (o >= e)
				break;
			ln = *o++;
			if (o + ln > e)
				break;
			switch (c) {
			case 53: type = o[0]; break;
			case 1: memcpy(&l->mask, o, 4); break;
			case 3: memcpy(&l->gw, o, 4); break;
			case 6:
				memcpy(&l->dns1, o, 4);
				if (ln >= 8)
					memcpy(&l->dns2, o + 4, 4);
				break;
			case 54: memcpy(&l->server, o, 4); break;
			case 51: { uint32_t v; memcpy(&v, o, 4); l->secs = ntohl(v); break; }
			}
			o += ln;
		}
		return type;
	}
}

static int dhcp_tries = 5;
static struct lease g_lease;
static int g_have_ip;

static int do_dhcp(const char *ifname)
{
	struct ifreq ifr;
	struct sockaddr_ll sll;
	struct lease l;
	uint8_t mac[6];
	uint32_t xid;
	int s, fd, ifindex, tries, t = -1;
	char ipb[16], mb[16], gb[16], d1[16];

	if_up(ifname);
	s = ctl_sock();
	if (s < 0)
		return 1;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0) {
		perror("SIOCGIFHWADDR");
		return 1;
	}
	memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);		/* copy before the next ioctl: ifr_ifindex overlays sa_data */
	if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
		perror("SIOCGIFINDEX");
		return 1;
	}
	ifindex = ifr.ifr_ifindex;
	close(s);
	printf("dhcp: %s idx %d mac %02x:%02x:%02x:%02x:%02x:%02x\n", ifname, ifindex,
	       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	fd = socket(PF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
	if (fd < 0) {
		perror("packet socket");
		return 1;
	}
	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_IP);
	sll.sll_ifindex = ifindex;
	if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
		perror("bind");
		return 1;
	}
	xid = (uint32_t)time(NULL) ^ (mac[4] << 8 | mac[5]) ^ 0x50535000;
	for (tries = 0; tries < dhcp_tries; tries++) {
		printf("dhcp: DISCOVER (try %d)\n", tries + 1);
		if (dhcp_send(fd, ifindex, mac, xid, 1, 0, 0) < 0)
			perror("send");
		t = dhcp_recv(fd, xid, &l, 4);
		if (t == 2)
			break;
	}
	if (t != 2) {
		printf("dhcp: no OFFER\n");
		return 2;
	}
	inet_ntop(AF_INET, &l.ip, ipb, sizeof(ipb));
	inet_ntop(AF_INET, &l.server, gb, sizeof(gb));
	printf("dhcp: OFFER %s from %s\n", ipb, gb);
	{
		uint32_t offer = l.ip, server = l.server;

		for (tries = 0; tries < 4; tries++) {
			dhcp_send(fd, ifindex, mac, xid, 3, offer, server);
			t = dhcp_recv(fd, xid, &l, 4);
			if (t == 5)
				break;
			if (t == 6) {
				printf("dhcp: NAK\n");
				return 3;
			}
		}
		if (t != 5) {
			printf("dhcp: no ACK\n");
			return 3;
		}
	}
	close(fd);
	if (!l.mask)
		l.mask = htonl(0xffffff00);
	inet_ntop(AF_INET, &l.ip, ipb, sizeof(ipb));
	inet_ntop(AF_INET, &l.mask, mb, sizeof(mb));
	inet_ntop(AF_INET, &l.gw, gb, sizeof(gb));
	inet_ntop(AF_INET, &l.dns1, d1, sizeof(d1));
	printf("dhcp: ACK ip %s mask %s gw %s dns %s lease %us\n", ipb, mb, gb, d1, l.secs);
	g_lease = l;
	g_have_ip = 1;
	if_addr(ifname, l.ip, l.mask);
	if (l.gw)
		add_gw(l.gw);
	{
		FILE *f = fopen("/etc/resolv.conf", "w");

		if (f) {
			if (l.dns1) {
				inet_ntop(AF_INET, &l.dns1, d1, sizeof(d1));
				fprintf(f, "nameserver %s\n", d1);
			}
			if (l.dns2) {
				inet_ntop(AF_INET, &l.dns2, d1, sizeof(d1));
				fprintf(f, "nameserver %s\n", d1);
			}
			if (!l.dns1)
				fprintf(f, "nameserver %s\n", gb);
			fclose(f);
		}
	}
	return 0;
}

/* ---------------- ping ---------------- */

static int do_ping(const char *host, int count)
{
	struct sockaddr_in to;
	struct hostent *he;
	int s, i, ok = 0;
	uint8_t pkt[64];

	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	if (!inet_aton(host, &to.sin_addr)) {
		he = gethostbyname(host);
		if (!he || he->h_addrtype != AF_INET) {
			printf("ping: cannot resolve %s\n", host);
			return 1;
		}
		memcpy(&to.sin_addr, he->h_addr, 4);
	}
	s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (s < 0) {
		perror("raw socket");
		return 1;
	}
	printf("ping %s\n", inet_ntoa(to.sin_addr));
	for (i = 0; i < count; i++) {
		struct timeval t0, t1;
		fd_set rf;
		struct timeval tv = { 3, 0 };
		uint8_t rb[256];

		memset(pkt, 0, sizeof(pkt));
		pkt[0] = 8;
		pkt[4] = 0x50; pkt[5] = 0x53; pkt[6] = i >> 8; pkt[7] = i;
		memset(pkt + 8, 0x42, 56);
		{
			uint16_t c = ip_csum(pkt, 64);

			memcpy(pkt + 2, &c, 2);
		}
		gettimeofday(&t0, NULL);
		sendto(s, pkt, 64, 0, (struct sockaddr *)&to, sizeof(to));
		for (;;) {
			int n, ihl;

			FD_ZERO(&rf);
			FD_SET(s, &rf);
			if (select(s + 1, &rf, NULL, NULL, &tv) <= 0) {
				printf("seq %d: timeout\n", i);
				break;
			}
			n = recv(s, rb, sizeof(rb), 0);
			ihl = (rb[0] & 15) * 4;
			if (n > ihl + 8 && rb[ihl] == 0 && rb[ihl + 4] == 0x50 && rb[ihl + 5] == 0x53 &&
			    rb[ihl + 7] == (i & 0xff)) {
				long ms;

				gettimeofday(&t1, NULL);
				ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000;
				printf("seq %d: reply %d bytes ttl %d time %ld ms\n", i, n - ihl, rb[8], ms);
				ok++;
				break;
			}
		}
		if (i + 1 < count)
			sleep(1);
	}
	printf("ping: %d/%d replies\n", ok, count);
	close(s);
	return ok ? 0 : 1;
}

/* ---------------- http get ---------------- */

static int do_get(const char *host, const char *path, int port)
{
	struct sockaddr_in a;
	struct hostent *he;
	char req[512], buf[1024];
	int s, n, total = 0;

	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	if (!inet_aton(host, &a.sin_addr)) {
		he = gethostbyname(host);
		if (!he || he->h_addrtype != AF_INET) {
			printf("get: cannot resolve %s\n", host);
			return 1;
		}
		memcpy(&a.sin_addr, he->h_addr, 4);
	}
	printf("get: %s (%s) port %d\n", host, inet_ntoa(a.sin_addr), port);
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		perror("socket");
		return 1;
	}
	{
		struct timeval tmo = { 10, 0 };

		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
		setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof(tmo));
	}
	if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		perror("connect");
		return 1;
	}
	snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: pnet\r\nConnection: close\r\n\r\n",
		 path, host);
	write(s, req, strlen(req));
	while ((n = read(s, buf, sizeof(buf))) > 0) {
		if (total < 3072) {
			int k = 3072 - total < n ? 3072 - total : n;

			fwrite(buf, 1, k, stdout);
		}
		total += n;
	}
	printf("\nget: %d bytes total\n", total);
	close(s);
	return total > 0 ? 0 : 1;
}


/* ---------------- sniff: dump frames the kernel receives on an interface ---------------- */

static int do_sniff(const char *ifname, int secs)
{
	struct ifreq ifr;
	struct sockaddr_ll sll;
	static uint8_t buf[2048];
	int s = ctl_sock(), fd, n = 0;
	time_t end;

	if (s < 0)
		return 1;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
		perror("SIOCGIFINDEX");
		return 1;
	}
	close(s);
	fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd < 0) {
		perror("packet socket");
		return 1;
	}
	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_ALL);
	sll.sll_ifindex = ifr.ifr_ifindex;
	if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
		perror("bind");
		return 1;
	}
	end = time(NULL) + secs;
	printf("sniff: %s for %d s\n", ifname, secs);
	while (time(NULL) < end) {
		fd_set rf;
		struct timeval tv = { 1, 0 };
		int len, i, show;

		FD_ZERO(&rf);
		FD_SET(fd, &rf);
		if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0)
			continue;
		len = recv(fd, buf, sizeof(buf), 0);
		if (len < 14)
			continue;
		n++;
		show = len < 256 ? len : 256;
		printf("rx %d: len %d type %02x%02x ", n, len, buf[12], buf[13]);
		for (i = 0; i < show; i++)
			printf("%02x", buf[i]);
		printf("\n");
	}
	printf("sniff: %d frames\n", n);
	return 0;
}

/* ---------------- WPA2-PSK supplicant ---------------- */

static void psp_cmd(const char *s)
{
	int fd = open("/proc/psp_wlan", O_WRONLY);

	if (fd < 0) {
		perror("/proc/psp_wlan");
		return;
	}
	write(fd, s, strlen(s));
	close(fd);
}

/* load a 16-byte key into the chip: cmd 0x34 / 0x35, payload = u16 action + 32-byte key slot */
static void install_key(int cmd, int action, const u8 *key16, int keylen)
{
	char line[200];
	int n, i;
	u8 blob[32];

	memset(blob, 0, sizeof(blob));
	memcpy(blob, key16, keylen);
	n = snprintf(line, sizeof(line), "cmdx %x 2a %02x %02x", cmd, action & 0xff, action >> 8);
	for (i = 0; i < 32; i++)
		n += snprintf(line + n, sizeof(line) - n, " %02x", blob[i]);
	psp_cmd(line);
	printf("wpa: installed key with cmd 0x%02x action %d\n", cmd, action);
}

/* pairwise key: cmd 0x34 action 3, group key: cmd 0x35 action 5 (CCMP) / 3 (TKIP, untested).
 * Confirmed on hardware: variant 1.  Variants 2-4 are kept only as experiments. */
static void install_variant(int v, const u8 *tk, const u8 *gtk, int gtklen, int gcipher)
{
	int gact = gcipher == 2 ? 3 : 5;

	switch (v) {
	case 2: install_key(0x34, 1, tk, 16); install_key(0x35, gact, gtk, gtklen); break;
	case 3: install_key(0x35, 5, tk, 16); install_key(0x34, 3, gtk, gtklen); break;
	case 4: install_key(0x35, 3, tk, 16); install_key(0x35, gact, gtk, gtklen); break;
	default: install_key(0x34, 3, tk, 16); install_key(0x35, gact, gtk, gtklen); break;
	}
}

static int read_conf(const char *path, char *ssid, int ssz, char *psk, int psz)
{
	char line[200];
	FILE *f = fopen(path, "r");

	ssid[0] = psk[0] = 0;
	if (!f) {
		perror(path);
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		char *e = line + strlen(line);

		while (e > line && (e[-1] == '\n' || e[-1] == '\r'))
			*--e = 0;
		if (!strncmp(line, "ssid=", 5))
			strncpy(ssid, line + 5, ssz - 1);
		else if (!strncmp(line, "psk=", 4))
			strncpy(psk, line + 4, psz - 1);
	}
	fclose(f);
	return ssid[0] && psk[0] ? 0 : -1;
}

static int wpa_handshake(const char *ifname, const u8 *pmk, const u8 *rsn, int rsnlen, int gcipher,
			 int variant, int timeout)
{
	u8 ptk[48], snonce[32], aa[6], mac[6], gtk[32], replay[8], lastrp[8];
	static u8 fr[2048], out[1600], kd[1024];
	struct ifreq ifr;
	struct sockaddr_ll sll;
	struct timeval t1;
	int s, fd, ifindex, have_ptk = 0, done = 0, nmsg1 = 0, nmsg3 = 0, gtklen = 0, keyid = 0;
	time_t end;

	if_up(ifname);
	s = ctl_sock();
	if (s < 0)
		return 1;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0) {
		perror("SIOCGIFHWADDR");
		return 1;
	}
	memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
	if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
		perror("SIOCGIFINDEX");
		return 1;
	}
	ifindex = ifr.ifr_ifindex;
	close(s);

	fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_PAE));
	if (fd < 0) {
		perror("packet socket");
		return 1;
	}
	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_PAE);
	sll.sll_ifindex = ifindex;
	if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
		perror("bind");
		return 1;
	}
	{	/* snonce: not secret-critical, but unique per run */
		u8 seed[40];
		int i;

		gettimeofday(&t1, NULL);
		memcpy(seed, &t1, 8);
		memcpy(seed + 8, mac, 6);
		memset(seed + 14, 0, 26);
		for (i = 0; i < 2; i++) {
			u8 h[20];

			seed[39] = i;
			hmac_sha1(pmk, 32, seed, 40, h);
			memcpy(snonce + 16 * i, h, 16);
		}
	}
	memset(lastrp, 0, 8);
	end = time(NULL) + timeout;
	while (!done && time(NULL) < end) {
		fd_set rf;
		struct timeval tv = { 1, 0 };
		struct eapol_key k;
		int n, len;

		FD_ZERO(&rf);
		FD_SET(fd, &rf);
		if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0)
			continue;
		len = recv(fd, fr, sizeof(fr), 0);
		if (len < 14 + EAPOL_HDR || !memcmp(fr + 6, mac, 6))
			continue;		/* too short, or our own transmit */
		if (eapol_parse(fr + 14, len - 14, &k) < 0 || k.type != 2)
			continue;
		if ((k.info & 0x0188) == 0x0088) {		/* message 1 */
			nmsg1++;
			memcpy(aa, fr + 6, 6);
			wpa_ptk(pmk, aa, mac, k.nonce, snonce, ptk);
			have_ptk = 1;
			memcpy(replay, k.replay, 8);
			n = eapol_build(out + 14, 2, replay, snonce, ptk, rsn, rsnlen);
			memcpy(out, aa, 6);
			memcpy(out + 6, mac, 6);
			out[12] = 0x88; out[13] = 0x8e;
			{
				struct sockaddr_ll to = sll;

				to.sll_halen = 6;
				memcpy(to.sll_addr, aa, 6);
				if (sendto(fd, out, 14 + n, 0, (struct sockaddr *)&to, sizeof(to)) < 0)
					perror("send msg2");
			}
			printf("wpa: msg1 from %02x:%02x:%02x:%02x:%02x:%02x (#%d), sent msg2 (%d bytes)\n",
			       aa[0], aa[1], aa[2], aa[3], aa[4], aa[5], nmsg1, 14 + n);
		} else if ((k.info & 0x01c8) == 0x01c8 && have_ptk) {	/* message 3 */
			nmsg3++;
			if (eapol_check_mic(fr + 14, len - 14, ptk)) {
				printf("wpa: msg3 MIC check FAILED (wrong passphrase or PTK mismatch)\n");
				close(fd);
				return 3;
			}
			if (memcmp(k.replay, lastrp, 8) <= 0 && nmsg3 > 1)
				continue;
			memcpy(lastrp, k.replay, 8);
			if (k.datalen >= 24 && (k.info & 0x1000)) {
				if (aes_unwrap(ptk + 16, k.data, k.datalen, kd)) {
					printf("wpa: msg3 key data unwrap failed\n");
					close(fd);
					return 3;
				}
				if (wpa_find_gtk(kd, k.datalen - 8, gtk, &gtklen, &keyid))
					printf("wpa: no GTK in msg3\n");
				else
					printf("wpa: GTK id %d len %d\n", keyid, gtklen);
			}
			n = eapol_build(out + 14, 4, k.replay, snonce, ptk, NULL, 0);
			memcpy(out, aa, 6);
			memcpy(out + 6, mac, 6);
			out[12] = 0x88; out[13] = 0x8e;
			{
				struct sockaddr_ll to = sll;

				to.sll_halen = 6;
				memcpy(to.sll_addr, aa, 6);
				if (sendto(fd, out, 14 + n, 0, (struct sockaddr *)&to, sizeof(to)) < 0)
					perror("send msg4");
			}
			printf("wpa: msg3 OK (MIC verified), sent msg4\n");
			usleep(150000);
			install_variant(variant, ptk + 32, gtk, gtklen > 0 ? gtklen : 16, gcipher);
			done = 1;
		}
	}
	close(fd);
	if (!done) {
		printf("wpa: handshake incomplete (msg1 seen %d, msg3 seen %d)\n", nmsg1, nmsg3);
		return nmsg1 ? 2 : 1;
	}
	printf("WPA_OK\n");
	return 0;
}

static const u8 default_rsn[22] = { 0x30, 0x14, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4,
				    1, 0, 0, 0x0f, 0xac, 2, 0, 0 };

/* CLI: pnet wpa <if> <conf> [variant]  (association must already be done) */
static int do_wpa(const char *ifname, const char *conf, int variant)
{
	char ssid[40], psk[80];
	u8 pmk[32];

	if (read_conf(conf, ssid, sizeof(ssid), psk, sizeof(psk)) < 0) {
		printf("wpa: bad config %s\n", conf);
		return 1;
	}
	wpa_pmk(psk, (u8 *)ssid, strlen(ssid), pmk);
	return wpa_handshake(ifname, pmk, default_rsn, 22, 4, variant, 25);
}

/* ---------------- connect: link + WPA + DHCP + watchdog ---------------- */

/* seconds since boot; integer parsing only: the PSP kernel must not see FPU instructions from userland */
static long uptime_s(void)
{
	char b[64];
	int fd = open("/proc/uptime", O_RDONLY), n;
	long v = 0;
	int i;

	if (fd < 0)
		return 0;
	n = read(fd, b, sizeof(b) - 1);
	close(fd);
	for (i = 0; i < n && b[i] >= '0' && b[i] <= '9'; i++)
		v = v * 10 + (b[i] - '0');
	return v;
}

#define LOGT(fmt, ...) do { printf("[%5ld] " fmt, uptime_s(), ##__VA_ARGS__); fflush(stdout); } while (0)

/* write a command to the driver and read its log back (the driver executes it synchronously) */
static int proc_cmd(const char *cmd, char *res, int rsz)
{
	int fd, n = 0, k;

	fd = open("/proc/psp_wlan", O_WRONLY);
	if (fd < 0)
		return -1;
	write(fd, cmd, strlen(cmd));
	close(fd);
	if (!res)
		return 0;
	fd = open("/proc/psp_wlan", O_RDONLY);
	if (fd < 0)
		return -1;
	while (n < rsz - 1 && (k = read(fd, res + n, rsz - 1 - n)) > 0)
		n += k;
	res[n] = 0;
	close(fd);
	return n;
}

static int unhex1(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* is the gateway still answering ARP?  returns 1 when a reply for gw arrived */
static int arp_probe(const char *ifname, uint32_t myip, uint32_t gw)
{
	struct ifreq ifr;
	struct sockaddr_ll sll;
	u8 mac[6], req[42], rb[256];
	int s = ctl_sock(), fd, ifindex, got = 0, tries;

	if (s < 0)
		return 0;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0)
		return 0;
	memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
	if (ioctl(s, SIOCGIFINDEX, &ifr) < 0)
		return 0;
	ifindex = ifr.ifr_ifindex;
	close(s);
	fd = socket(PF_PACKET, SOCK_RAW, htons(0x0806));
	if (fd < 0)
		return 0;
	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(0x0806);
	sll.sll_ifindex = ifindex;
	sll.sll_halen = 6;
	if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
		close(fd);
		return 0;
	}
	memset(req, 0, sizeof(req));
	memset(req, 0xff, 6);
	memcpy(req + 6, mac, 6);
	req[12] = 0x08; req[13] = 0x06;
	req[14] = 0; req[15] = 1; req[16] = 8; req[17] = 0; req[18] = 6; req[19] = 4; req[21] = 1;
	memcpy(req + 22, mac, 6);
	memcpy(req + 28, &myip, 4);
	memcpy(req + 38, &gw, 4);
	for (tries = 0; tries < 3 && !got; tries++) {
		time_t end = time(NULL) + 1;

		sendto(fd, req, sizeof(req), 0, (struct sockaddr *)&sll, sizeof(sll));
		while (!got && time(NULL) <= end) {
			fd_set rf;
			struct timeval tv = { 0, 300000 };
			int n;

			FD_ZERO(&rf);
			FD_SET(fd, &rf);
			if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0)
				continue;
			n = recv(fd, rb, sizeof(rb), 0);
			if (n >= 42 && rb[21] == 2 && !memcmp(rb + 28, &gw, 4))	/* ARP reply from the gateway */
				got = 1;
		}
	}
	close(fd);
	return got;
}

/* one full connection attempt; returns 0 when we hold a DHCP lease over a working link */
static int connect_once(const char *ifname, const char *ssid, const char *psk, const u8 *pmk)
{
	static char res[4096];
	char cmd[96];
	char *p;
	u8 rsn[32];
	int rsnlen = 0, gcipher = 4, i;

	if (g_have_ip) {
		if_addr(ifname, 0, 0);
		g_have_ip = 0;
	}
	snprintf(cmd, sizeof(cmd), "link %s %s", psk[0] ? "wpa" : "open", ssid);
	if (proc_cmd(cmd, res, sizeof(res)) < 0) {
		LOGT("cannot talk to /proc/psp_wlan\n");
		return 1;
	}
	p = strstr(res, "LINK_OK");
	if (!p) {
		p = strstr(res, "LINK_FAIL");
		if (p) {
			char *e = strchr(p, '\n');

			if (e)
				*e = 0;
			LOGT("%s\n", p);
		} else {
			LOGT("link: no result from driver\n");
		}
		return 1;
	}
	{
		char *e = strchr(p, '\n');

		if (e)
			*e = 0;
	}
	LOGT("link ok: %.70s\n", p + 8);
	proc_cmd("netup", NULL, 0);		/* registers wlan0 + starts the poll thread (idempotent) */
	if (psk[0]) {
		char *g = strstr(p, "gcipher="), *r = strstr(p, "rsn=");

		if (g)
			gcipher = atoi(g + 8);
		if (!r) {
			LOGT("driver gave no RSN IE\n");
			return 1;
		}
		r += 4;
		for (i = 0; i < 32 && unhex1(r[2 * i]) >= 0 && unhex1(r[2 * i + 1]) >= 0; i++)
			rsn[i] = unhex1(r[2 * i]) << 4 | unhex1(r[2 * i + 1]);
		rsnlen = i;
		if (wpa_handshake(ifname, pmk, rsn, rsnlen, gcipher, 1, 20)) {
			LOGT("WPA handshake failed\n");
			return 1;
		}
		LOGT("WPA handshake done\n");
	}
	dhcp_tries = 3;
	if (do_dhcp(ifname)) {
		LOGT("DHCP failed\n");
		return 1;
	}
	return 0;
}

static int do_connect(const char *ifname, const char *conf)
{
	char ssid[40], psk[80];
	u8 pmk[32];
	int fails = 0;

	if (read_conf(conf, ssid, sizeof(ssid), psk, sizeof(psk)) < 0 && !ssid[0]) {
		printf("connect: bad config %s (need ssid=, optional psk=)\n", conf);
		return 1;
	}
	LOGT("connect: ssid '%s' %s\n", ssid, psk[0] ? "WPA2-PSK" : "open");
	if (psk[0])
		wpa_pmk(psk, (u8 *)ssid, strlen(ssid), pmk);
	LOGT("connect: PMK ready\n");
	for (;;) {
		long up_t, lease_t;
		uint32_t myip, gw;
		int bad = 0;

		if (connect_once(ifname, ssid, psk, pmk)) {
			int wait = ++fails * 5;

			if (wait > 30)
				wait = 30;
			LOGT("attempt failed (%d in a row), retry in %d s\n", fails, wait);
			sleep(wait);
			continue;
		}
		fails = 0;
		myip = g_lease.ip;
		gw = g_lease.gw;
		up_t = lease_t = uptime_s();
		LOGT("CONNECTED\n");
		while (bad < 3) {
			sleep(5);
			if (access("/ms0/net/simloss", F_OK) == 0) {		/* test hook */
				unlink("/ms0/net/simloss");
				LOGT("simloss flag found: pretending the link is gone\n");
				bad = 3;
				break;
			}
			if (gw && arp_probe(ifname, myip, gw)) {
				bad = 0;
			} else {
				bad++;
				LOGT("gateway not answering (%d/3)\n", bad);
			}
			if (g_lease.secs && uptime_s() - lease_t > (long)g_lease.secs / 2) {
				dhcp_tries = 2;
				if (!do_dhcp(ifname)) {
					lease_t = uptime_s();
					myip = g_lease.ip;
					gw = g_lease.gw;
					LOGT("lease renewed\n");
				}
			}
		}
		LOGT("link lost after %ld s, reconnecting\n", uptime_s() - up_t);
	}
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc >= 3 && !strcmp(argv[1], "up"))
		return if_up(argv[2]) ? 1 : 0;
	if (argc >= 5 && !strcmp(argv[1], "addr"))
		return if_addr(argv[2], inet_addr(argv[3]), inet_addr(argv[4])) ? 1 : 0;
	if (argc >= 3 && !strcmp(argv[1], "gw"))
		return add_gw(inet_addr(argv[2])) ? 1 : 0;
	if (argc >= 3 && !strcmp(argv[1], "dhcp")) {
		if (argc > 3)
			dhcp_tries = atoi(argv[3]);
		return do_dhcp(argv[2]);
	}
	if (argc >= 3 && !strcmp(argv[1], "ping"))
		return do_ping(argv[2], argc > 3 ? atoi(argv[3]) : 3);
	if (argc >= 3 && !strcmp(argv[1], "get"))
		return do_get(argv[2], argc > 3 ? argv[3] : "/", argc > 4 ? atoi(argv[4]) : 80);
	if (argc >= 3 && !strcmp(argv[1], "sleep")) {		/* the rootfs busybox has no sleep */
		sleep(atoi(argv[2]));
		return 0;
	}
	if (argc >= 4 && !strcmp(argv[1], "connect"))
		return do_connect(argv[2], argv[3]);
	if (argc >= 4 && !strcmp(argv[1], "wpa"))
		return do_wpa(argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 1);
	if (argc >= 3 && !strcmp(argv[1], "sniff"))
		return do_sniff(argv[2], argc > 3 ? atoi(argv[3]) : 10);
	fprintf(stderr, "usage: pnet up|addr|gw|dhcp|ping|get ...\n");
	return 1;
}
