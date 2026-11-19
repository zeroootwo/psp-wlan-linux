/*
 * pnet ntp: set the system clock from an NTP server (SNTP, integer arithmetic only).
 * The PSP has no battery-backed clock that Linux can use: the date is 2000-01-01 after every boot.
 *
 * Copyright (C) 2026 Anastasiia Velikaya. GPL-2.0.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define NTP_TO_UNIX 2208988800UL

static int ntp_one(const char *host)
{
	struct sockaddr_in a;
	struct hostent *he;
	unsigned char pkt[48], rb[128];
	struct timeval tv;
	fd_set rf;
	int s, n, tries;

	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(123);
	if (!inet_aton(host, &a.sin_addr)) {
		he = gethostbyname(host);
		if (!he || he->h_addrtype != AF_INET)
			return -1;
		memcpy(&a.sin_addr, he->h_addr, 4);
	}
	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return -1;
	for (tries = 0; tries < 3; tries++) {
		memset(pkt, 0, sizeof(pkt));
		pkt[0] = 0x1b;			/* LI 0, version 3, client mode */
		if (sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&a, sizeof(a)) < 0)
			break;
		tv.tv_sec = 2;
		tv.tv_usec = 0;
		FD_ZERO(&rf);
		FD_SET(s, &rf);
		if (select(s + 1, &rf, NULL, NULL, &tv) <= 0)
			continue;
		n = recv(s, rb, sizeof(rb), 0);
		if (n >= 48 && (rb[0] & 7) == 4 && rb[1] != 0) {	/* server mode, synchronised */
			unsigned long secs = (unsigned long)rb[40] << 24 | rb[41] << 16 | rb[42] << 8 | rb[43];
			struct timeval set;
			time_t t;
			struct tm *g;

			if (secs <= NTP_TO_UNIX)
				break;
			set.tv_sec = secs - NTP_TO_UNIX;
			set.tv_usec = 0;
			t = set.tv_sec;
			g = gmtime(&t);
			printf("ntp: %s -> %04d-%02d-%02d %02d:%02d:%02d UTC\n", host, g->tm_year + 1900, g->tm_mon + 1,
			       g->tm_mday, g->tm_hour, g->tm_min, g->tm_sec);
			if (settimeofday(&set, NULL) < 0) {
				perror("settimeofday");
				close(s);
				return -2;
			}
			close(s);
			return 0;
		}
	}
	close(s);
	return -1;
}

int do_ntp(const char *server)
{
	static const char *def[] = { "pool.ntp.org", "time.cloudflare.com", "time.google.com", NULL };
	int i;

	if (server)
		return ntp_one(server) ? 1 : 0;
	for (i = 0; def[i]; i++)
		if (!ntp_one(def[i]))
			return 0;
	printf("ntp: no server answered\n");
	return 1;
}
