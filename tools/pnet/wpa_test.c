/* host-side self test: prints values for comparison against Python's hashlib/hmac/openssl */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wpa.h"

static void hex(const char *n, const u8 *b, int l) { int i; printf("%s=", n); for (i = 0; i < l; i++) printf("%02x", b[i]); printf("\n"); }
static int unhex(const char *s, u8 *o) { int n = 0; while (s[0] && s[1]) { unsigned v; sscanf(s, "%2x", &v); o[n++] = v; s += 2; } return n; }

int main(int argc, char **argv)
{
	u8 pmk[32], out[64], b[256];
	int n;

	if (argc > 1 && !strcmp(argv[1], "pmk")) {		/* pmk <pass> <ssid> */
		wpa_pmk(argv[2], (u8 *)argv[3], strlen(argv[3]), pmk);
		hex("pmk", pmk, 32);
	} else if (argc > 1 && !strcmp(argv[1], "hmac")) {	/* hmac <keyhex> <datahex> */
		u8 k[128], d[512];
		int kl = unhex(argv[2], k), dl = unhex(argv[3], d);
		hmac_sha1(k, kl, d, dl, out);
		hex("hmac", out, 20);
	} else if (argc > 1 && !strcmp(argv[1], "ptk")) {	/* ptk pmk aa spa anonce snonce */
		u8 a[6], s[6], an[32], sn[32], p[32];
		unhex(argv[2], p); unhex(argv[3], a); unhex(argv[4], s); unhex(argv[5], an); unhex(argv[6], sn);
		wpa_ptk(p, a, s, an, sn, out);
		hex("ptk", out, 48);
	} else if (argc > 1 && !strcmp(argv[1], "unwrap")) {	/* unwrap kek datahex */
		u8 k[16], d[256];
		int dl;
		unhex(argv[2], k); dl = unhex(argv[3], d);
		n = aes_unwrap(k, d, dl, b);
		printf("unwrap rc=%d\n", n);
		if (!n) hex("plain", b, dl - 8);
	} else if (argc > 1 && !strcmp(argv[1], "msg")) {	/* msg <2|4> replayhex snoncehex kckhex keydatahex */
		u8 rp[8], sn[32], kck[16], kd[256], f[512];
		int kl = 0;
		unhex(argv[3], rp); unhex(argv[4], sn); unhex(argv[5], kck);
		if (argc > 6) kl = unhex(argv[6], kd);
		n = eapol_build(f, atoi(argv[2]), rp, sn, kck, kd, kl);
		hex("frame", f, n);
		printf("micok=%d\n", eapol_check_mic(f, n, kck));
	}
	return 0;
}
