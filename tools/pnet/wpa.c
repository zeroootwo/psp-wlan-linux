/*
 * Minimal WPA2-PSK (CCMP, key descriptor version 2) supplicant primitives.
 * SHA-1, HMAC, PBKDF2, 802.11i PRF, AES-128 decrypt (key unwrap), EAPOL-Key build/parse.
 * No dependencies; builds for the PSP uClinux target and on a host for self-testing.
 *
 * Copyright (C) 2026 Anastasiia Velikaya. GPL-2.0.
 */
#include <string.h>
#include "wpa.h"

/* ---------------- SHA-1 ---------------- */

struct sha1 {
	uint32_t h[5];
	uint64_t len;
	u8 buf[64];
	int n;
};

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(struct sha1 *c, const u8 *p)
{
	uint32_t w[80], a, b, d, e, f, k, t, cc;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
	for (i = 16; i < 80; i++)
		w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
	for (i = 0; i < 80; i++) {
		if (i < 20) { f = (b & cc) | (~b & d); k = 0x5a827999; }
		else if (i < 40) { f = b ^ cc ^ d; k = 0x6ed9eba1; }
		else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8f1bbcdc; }
		else { f = b ^ cc ^ d; k = 0xca62c1d6; }
		t = ROL(a, 5) + f + e + k + w[i];
		e = d; d = cc; cc = ROL(b, 30); b = a; a = t;
	}
	c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1_init(struct sha1 *c)
{
	c->h[0] = 0x67452301; c->h[1] = 0xefcdab89; c->h[2] = 0x98badcfe;
	c->h[3] = 0x10325476; c->h[4] = 0xc3d2e1f0;
	c->len = 0;
	c->n = 0;
}

static void sha1_update(struct sha1 *c, const u8 *d, int n)
{
	c->len += n;
	while (n > 0) {
		int k = 64 - c->n < n ? 64 - c->n : n;

		memcpy(c->buf + c->n, d, k);
		c->n += k; d += k; n -= k;
		if (c->n == 64) {
			sha1_block(c, c->buf);
			c->n = 0;
		}
	}
}

static void sha1_final(struct sha1 *c, u8 out[20])
{
	uint64_t bits = c->len * 8;
	u8 pad = 0x80, z = 0, l[8];
	int i;

	sha1_update(c, &pad, 1);
	while (c->n != 56)
		sha1_update(c, &z, 1);
	for (i = 0; i < 8; i++)
		l[i] = bits >> (56 - 8 * i);
	sha1_update(c, l, 8);
	for (i = 0; i < 5; i++) {
		out[4 * i] = c->h[i] >> 24; out[4 * i + 1] = c->h[i] >> 16;
		out[4 * i + 2] = c->h[i] >> 8; out[4 * i + 3] = c->h[i];
	}
}

void hmac_sha1(const u8 *key, int klen, const u8 *data, int dlen, u8 out[20])
{
	u8 k[64], tmp[20], ipad[64], opad[64];
	struct sha1 c;
	int i;

	memset(k, 0, 64);
	if (klen > 64) {
		sha1_init(&c);
		sha1_update(&c, key, klen);
		sha1_final(&c, k);
	} else {
		memcpy(k, key, klen);
	}
	for (i = 0; i < 64; i++) {
		ipad[i] = k[i] ^ 0x36;
		opad[i] = k[i] ^ 0x5c;
	}
	sha1_init(&c);
	sha1_update(&c, ipad, 64);
	sha1_update(&c, data, dlen);
	sha1_final(&c, tmp);
	sha1_init(&c);
	sha1_update(&c, opad, 64);
	sha1_update(&c, tmp, 20);
	sha1_final(&c, out);
}

void pbkdf2_sha1(const u8 *pass, int plen, const u8 *salt, int slen, int iter, u8 *out, int outlen)
{
	u8 s[64 + 4], u[20], t[20];
	int blk = 1, i, j, n;

	while (outlen > 0) {
		memcpy(s, salt, slen);
		s[slen] = blk >> 24; s[slen + 1] = blk >> 16; s[slen + 2] = blk >> 8; s[slen + 3] = blk;
		hmac_sha1(pass, plen, s, slen + 4, u);
		memcpy(t, u, 20);
		for (i = 1; i < iter; i++) {
			hmac_sha1(pass, plen, u, 20, u);
			for (j = 0; j < 20; j++)
				t[j] ^= u[j];
		}
		n = outlen < 20 ? outlen : 20;
		memcpy(out, t, n);
		out += n; outlen -= n; blk++;
	}
}

/* IEEE 802.11i PRF: HMAC-SHA1(K, A || 0x00 || B || i) */
void wpa_prf(const u8 *key, int klen, const char *label, const u8 *data, int dlen, u8 *out, int outlen)
{
	u8 buf[128], h[20];
	int ll = strlen(label), i = 0, n;

	memcpy(buf, label, ll);
	buf[ll] = 0;
	memcpy(buf + ll + 1, data, dlen);
	while (outlen > 0) {
		buf[ll + 1 + dlen] = i++;
		hmac_sha1(key, klen, buf, ll + 2 + dlen, h);
		n = outlen < 20 ? outlen : 20;
		memcpy(out, h, n);
		out += n; outlen -= n;
	}
}

/* ---------------- AES-128 (decrypt only) ---------------- */

#define ROL8(x, n) ((u8)(((x) << (n)) | ((x) >> (8 - (n)))))

static u8 sbox[256], rsbox[256];
static int aes_ready;

static u8 xt(u8 x) { return (x << 1) ^ ((x >> 7) * 0x1b); }

static u8 gmul(u8 a, u8 b)
{
	u8 r = 0;

	while (b) {
		if (b & 1)
			r ^= a;
		a = xt(a);
		b >>= 1;
	}
	return r;
}

static void aes_init(void)
{
	int i;
	u8 p = 1, q = 1;

	if (aes_ready)
		return;
	do {					/* generate the S-box (standard construction) */
		u8 x;

		p = p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0);
		q ^= q << 1; q ^= q << 2; q ^= q << 4;
		if (q & 0x80)
			q ^= 0x09;
		x = q ^ ROL8(q, 1) ^ ROL8(q, 2) ^ ROL8(q, 3) ^ ROL8(q, 4);
		sbox[p] = x ^ 0x63;
	} while (p != 1);
	sbox[0] = 0x63;
	for (i = 0; i < 256; i++)
		rsbox[sbox[i]] = i;
	aes_ready = 1;
}
#undef ROL

static void aes_expand(const u8 *key, u8 rk[176])
{
	int i;
	u8 rc = 1;

	memcpy(rk, key, 16);
	for (i = 16; i < 176; i += 4) {
		u8 t[4];

		memcpy(t, rk + i - 4, 4);
		if (i % 16 == 0) {
			u8 x = t[0];

			t[0] = sbox[t[1]] ^ rc; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[x];
			rc = xt(rc);
		}
		rk[i] = rk[i - 16] ^ t[0]; rk[i + 1] = rk[i - 15] ^ t[1];
		rk[i + 2] = rk[i - 14] ^ t[2]; rk[i + 3] = rk[i - 13] ^ t[3];
	}
}

static void aes_dec_block(const u8 rk[176], const u8 in[16], u8 out[16])
{
	u8 s[16], t[16];
	int r, i;

	for (i = 0; i < 16; i++)
		s[i] = in[i] ^ rk[160 + i];
	for (r = 9; r >= 0; r--) {
		/* inverse shift rows */
		t[0] = s[0]; t[4] = s[4]; t[8] = s[8]; t[12] = s[12];
		t[1] = s[13]; t[5] = s[1]; t[9] = s[5]; t[13] = s[9];
		t[2] = s[10]; t[6] = s[14]; t[10] = s[2]; t[14] = s[6];
		t[3] = s[7]; t[7] = s[11]; t[11] = s[15]; t[15] = s[3];
		for (i = 0; i < 16; i++)
			s[i] = rsbox[t[i]] ^ rk[16 * r + i];
		if (r > 0) {			/* inverse mix columns */
			for (i = 0; i < 16; i += 4) {
				u8 a = s[i], b = s[i + 1], c = s[i + 2], d = s[i + 3];

				s[i] = gmul(a, 14) ^ gmul(b, 11) ^ gmul(c, 13) ^ gmul(d, 9);
				s[i + 1] = gmul(a, 9) ^ gmul(b, 14) ^ gmul(c, 11) ^ gmul(d, 13);
				s[i + 2] = gmul(a, 13) ^ gmul(b, 9) ^ gmul(c, 14) ^ gmul(d, 11);
				s[i + 3] = gmul(a, 11) ^ gmul(b, 13) ^ gmul(c, 9) ^ gmul(d, 14);
			}
		}
	}
	memcpy(out, s, 16);
}

/* RFC 3394 unwrap; inlen multiple of 8, >= 24; out gets inlen-8 bytes; 0 = ok */
int aes_unwrap(const u8 *kek16, const u8 *in, int inlen, u8 *out)
{
	u8 rk[176], a[8], b[16];
	int n = inlen / 8 - 1, i, j;

	if (inlen < 24 || inlen % 8)
		return -1;
	aes_init();
	aes_expand(kek16, rk);
	memcpy(a, in, 8);
	memcpy(out, in + 8, 8 * n);
	for (j = 5; j >= 0; j--) {
		for (i = n; i >= 1; i--) {
			uint32_t t = n * j + i;

			memcpy(b, a, 8);
			b[7] ^= t; b[6] ^= t >> 8; b[5] ^= t >> 16; b[4] ^= t >> 24;
			memcpy(b + 8, out + 8 * (i - 1), 8);
			aes_dec_block(rk, b, b);
			memcpy(a, b, 8);
			memcpy(out + 8 * (i - 1), b + 8, 8);
		}
	}
	for (i = 0; i < 8; i++)
		if (a[i] != 0xa6)
			return -2;
	return 0;
}

/* ---------------- WPA2 ---------------- */

void wpa_pmk(const char *pass, const u8 *ssid, int slen, u8 pmk[32])
{
	pbkdf2_sha1((const u8 *)pass, strlen(pass), ssid, slen, 4096, pmk, 32);
}

void wpa_ptk(const u8 pmk[32], const u8 *aa, const u8 *spa, const u8 *anonce, const u8 *snonce, u8 ptk[48])
{
	u8 d[76];
	const u8 *lo, *hi;

	lo = memcmp(aa, spa, 6) < 0 ? aa : spa;
	hi = lo == aa ? spa : aa;
	memcpy(d, lo, 6); memcpy(d + 6, hi, 6);
	lo = memcmp(anonce, snonce, 32) < 0 ? anonce : snonce;
	hi = lo == anonce ? snonce : anonce;
	memcpy(d + 12, lo, 32); memcpy(d + 44, hi, 32);
	wpa_prf(pmk, 32, "Pairwise key expansion", d, 76, ptk, 48);
}

int eapol_parse(const u8 *f, int len, struct eapol_key *k)
{
	int bl;

	if (len < EAPOL_HDR || f[1] != 3)
		return -1;
	bl = f[2] << 8 | f[3];
	if (4 + bl > len)
		return -1;
	k->type = f[4];
	k->info = f[5] << 8 | f[6];
	k->keylen = f[7] << 8 | f[8];
	memcpy(k->replay, f + 9, 8);
	memcpy(k->nonce, f + 17, 32);
	memcpy(k->mic, f + 81, 16);
	k->datalen = f[97] << 8 | f[98];
	k->data = f + 99;
	if (99 + k->datalen > len)
		return -1;
	return 0;
}

int eapol_build(u8 *o, int msg, const u8 *replay, const u8 *snonce, const u8 *kck,
		const u8 *kd, int kdlen)
{
	u8 mic[20];
	uint16_t info = msg == 2 ? 0x010a : 0x030a;	/* ver2 | pairwise | MIC [| secure] */
	int body = 95 + kdlen;

	memset(o, 0, EAPOL_HDR + kdlen);
	o[0] = 2; o[1] = 3; o[2] = body >> 8; o[3] = body;
	o[4] = 2;
	o[5] = info >> 8; o[6] = info;
	o[7] = 0; o[8] = msg == 2 ? 0 : 0;		/* key length 0 in msg2/4 */
	memcpy(o + 9, replay, 8);
	if (msg == 2)
		memcpy(o + 17, snonce, 32);
	o[97] = kdlen >> 8; o[98] = kdlen;
	if (kdlen)
		memcpy(o + 99, kd, kdlen);
	hmac_sha1(kck, 16, o, EAPOL_HDR + kdlen, mic);
	memcpy(o + 81, mic, 16);
	return EAPOL_HDR + kdlen;
}

int eapol_check_mic(const u8 *f, int len, const u8 *kck)
{
	u8 tmp[2048], mic[20];
	int bl = f[2] << 8 | f[3], n = 4 + bl;

	if (n > len || n > (int)sizeof(tmp))
		return -1;
	memcpy(tmp, f, n);
	memset(tmp + 81, 0, 16);
	hmac_sha1(kck, 16, tmp, n, mic);
	return memcmp(mic, f + 81, 16) ? -2 : 0;
}

int wpa_find_gtk(const u8 *kd, int len, u8 *gtk, int *gtklen, int *keyid)
{
	int i = 0;

	while (i + 2 <= len) {
		int t = kd[i], l = kd[i + 1];

		if (t == 0 && l == 0)
			break;
		if (i + 2 + l > len)
			return -1;
		if (t == 0xdd && l >= 6 && kd[i + 2] == 0x00 && kd[i + 3] == 0x0f &&
		    kd[i + 4] == 0xac && kd[i + 5] == 0x01) {
			*keyid = kd[i + 6] & 3;
			*gtklen = l - 6;
			memcpy(gtk, kd + i + 8, *gtklen);
			return 0;
		}
		i += 2 + l;
	}
	return -1;
}
