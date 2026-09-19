/*
 * modexp 对拍工具：stdin 每行一条消息（hex），stdout 每行一条 256 字符密文 hex。
 * 给 test/modexp-diff.py 用 Python 的整数幂做随机差分（CI 里跑，不需要 OpenSSL）。
 * 编译： cc -O2 -o modexp_cli test/modexp_cli.c
 */
#include <stdio.h>
#include <string.h>

#include "../src/modexp.h"

#define MODULUS \
	"94dd2a8675fb779e6b9f7103698634cd400f27a154afa67af6166a43fc2641722" \
	"2a79506d34cacc7641946abda1785b7acf9910ad6a0978c91ec84d40b71d2891" \
	"379af19ffb333e7517e390bd26ac312fe940c340466b4a5d4af1d65c3b5944078" \
	"f96a1a51a5a53e4bc302818b7c9f63c4a1b07bd7d874cef1c3d4b2f5eb7871"
#define EXPONENT "10001"

int main(void)
{
	char line[MODEXP_DBL * 2 + 8];
	char out[MODEXP_HEX + 1];
	unsigned char msg[MODEXP_DBL];
	size_t len, n, i;
	int hi;

	while (fgets(line, sizeof(line), stdin)) {
		len = strlen(line);
		while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';

		if (!len)
			continue;

		n = 0;
		hi = -1;

		for (i = 0; i < len; i++) {
			int v = modexp_hexval(line[i]);

			if (v < 0) {
				fprintf(stderr, "bad hex at %zu\n", i);
				return 1;
			}

			if (hi < 0) {
				hi = v;
			}
			else {
				if (n >= sizeof(msg))
					return 1;

				msg[n++] = (unsigned char)((hi << 4) | v);
				hi = -1;
			}
		}

		if (hi >= 0) {
			if (n >= sizeof(msg))
				return 1;

			msg[n++] = (unsigned char)hi;
		}

		modexp_pow_hex(out, msg, n, MODULUS, EXPONENT);
		puts(out);
	}

	return 0;
}
