/*
 * memtest MB: allocate MB megabytes in 1 MB chunks, fill them with an address-dependent pattern, verify, then
 * verify again with the inverted pattern.  Integer code only (no FPU).  Prints a one-line summary.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXCH 128
static unsigned int *ch[MAXCH];

static unsigned int pat(unsigned long a, int pass)
{
	unsigned int v = (unsigned int)a * 2654435761U ^ (unsigned int)(a >> 7);

	return pass ? ~v : v;
}

int main(int argc, char **argv)
{
	int want = argc > 1 ? atoi(argv[1]) : 16, n = 0, i, pass;
	unsigned long errors = 0, words = 0;

	if (want > MAXCH)
		want = MAXCH;
	while (n < want) {
		ch[n] = malloc(1 << 20);
		if (!ch[n])
			break;
		n++;
	}
	printf("memtest: got %d of %d MB\n", n, want);
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < n; i++) {
			unsigned int *p = ch[i];
			unsigned long j, base = (unsigned long)p;

			for (j = 0; j < (1 << 20) / 4; j++)
				p[j] = pat(base + 4 * j, pass);
		}
		for (i = 0; i < n; i++) {
			unsigned int *p = ch[i];
			unsigned long j, base = (unsigned long)p;

			for (j = 0; j < (1 << 20) / 4; j++, words++)
				if (p[j] != pat(base + 4 * j, pass))
					errors++;
		}
	}
	printf("memtest: %lu words verified, %lu errors; lowest chunk %08lx highest chunk %08lx\n", words, errors,
	       n ? (unsigned long)ch[0] : 0UL, n ? (unsigned long)ch[n - 1] : 0UL);
	{
		unsigned long lo = ~0UL, hi = 0;

		for (i = 0; i < n; i++) {
			if ((unsigned long)ch[i] < lo)
				lo = (unsigned long)ch[i];
			if ((unsigned long)ch[i] > hi)
				hi = (unsigned long)ch[i];
		}
		printf("memtest: address range %08lx .. %08lx\n", lo, hi);
	}
	return errors != 0;
}
