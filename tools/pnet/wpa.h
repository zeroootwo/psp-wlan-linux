#ifndef PNET_WPA_H
#define PNET_WPA_H
#include <stdint.h>

typedef uint8_t u8;

/* primitives (exposed for the host-side self test) */
void hmac_sha1(const u8 *key, int klen, const u8 *data, int dlen, u8 out[20]);
void pbkdf2_sha1(const u8 *pass, int plen, const u8 *salt, int slen, int iter, u8 *out, int outlen);
void wpa_prf(const u8 *key, int klen, const char *label, const u8 *data, int dlen, u8 *out, int outlen);
int aes_unwrap(const u8 *kek16, const u8 *in, int inlen, u8 *out);

/* WPA2-PSK CCMP supplicant, one handshake */
void wpa_pmk(const char *pass, const u8 *ssid, int slen, u8 pmk[32]);
void wpa_ptk(const u8 pmk[32], const u8 *aa, const u8 *spa, const u8 *anonce, const u8 *snonce, u8 ptk[48]);

#define EAPOL_HDR 99			/* version..key data length */

/* parsed EAPOL-Key frame (starts at the EAPOL version byte) */
struct eapol_key {
	u8 type;			/* descriptor type */
	uint16_t info;
	uint16_t keylen;
	u8 replay[8];
	u8 nonce[32];
	u8 mic[16];
	int datalen;
	const u8 *data;
};

int eapol_parse(const u8 *f, int len, struct eapol_key *k);
/* build message 2 (flag=2) or 4 (flag=4); returns frame length */
int eapol_build(u8 *out, int msg, const u8 *replay, const u8 *snonce, const u8 *kck,
		const u8 *keydata, int kdlen);
/* verify the MIC of a received frame with KCK; 0 = ok */
int eapol_check_mic(const u8 *f, int len, const u8 *kck);
/* find the GTK KDE in decrypted msg3 key data; returns 0 and fills gtk/keyid */
int wpa_find_gtk(const u8 *kd, int len, u8 *gtk, int *gtklen, int *keyid);
#endif
