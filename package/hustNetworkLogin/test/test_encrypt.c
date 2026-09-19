/*
 * 加密自测：验证 encrypt_pass 与上游 Rust 版逐字节一致。
 * 编译运行：
 *   gcc test_encrypt.c -o test_encrypt -lcrypto && ./test_encrypt
 * 期望输出：两个 MATCH，退出码 0。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <openssl/bn.h>

#define MODULUS \
	"94dd2a8675fb779e6b9f7103698634cd400f27a154afa67af6166a43fc2641722" \
	"2a79506d34cacc7641946abda1785b7acf9910ad6a0978c91ec84d40b71d2891" \
	"379af19ffb333e7517e390bd26ac312fe940c340466b4a5d4af1d65c3b5944078" \
	"f96a1a51a5a53e4bc302818b7c9f63c4a1b07bd7d874cef1c3d4b2f5eb7871"
#define EXPONENT "10001"

static char *encrypt_pass(const char *msg)
{
	BIGNUM *e = NULL, *n = NULL, *m = NULL, *c = NULL;
	BN_CTX *ctx = BN_CTX_new();
	char *hex = NULL, *out = NULL;
	size_t len, zeros, i;

	c = BN_new();
	BN_hex2bn(&e, EXPONENT);
	BN_hex2bn(&n, MODULUS);
	m = BN_bin2bn((const unsigned char *)msg, (int)strlen(msg), NULL);
	BN_mod_exp(c, m, e, n, ctx);

	hex = BN_bn2hex(c);
	len = strlen(hex);
	zeros = (len < 256) ? (256 - len) : 0;
	out = malloc(257);
	memset(out, '0', zeros);
	for (i = 0; i < len; i++)
		out[zeros + i] = (char)tolower((unsigned char)hex[i]);
	out[256] = '\0';

	OPENSSL_free(hex);
	BN_free(e);
	BN_free(n);
	BN_free(m);
	BN_free(c);
	BN_CTX_free(ctx);
	return out;
}

int main(void)
{
	struct { const char *in; const char *expect; } cases[] = {
		{"123>5ae915bf808f82732e98e01f704f00cd",
		 "91a0e02175f6a0b22ad23dac0d7f599806bc091f9fee1bfdada0d24d011dcdaed418296b7c0ec560f988d92a7bb25dbf7ff51752d9bc6482a8180e56f7b772079ab59844abaae91e6d1c4660dc872717f9218f89acc9b70bb32891f28bf9d8f173d81b0e36c828deac919783e4e909ad1c22f953947b4a7ed7c90ac18fd95aa2"},
		{"197",
		 "0038c3a7a9719b65a89b82f56bdfc62c71f646403e169fbe1a391d8d1468e648e65e833174db7f1fad21e609ebd21432739e8ee7a3758938b4bd1d07390064918cf1763d6853525b761b055ae3dc229b1579eeacb7281ab258f2ea5c27455861503d814adb857000b24267fca4e70cac4e618f6258367367c0e43c2518e032d8"},
	};
	int i, fail = 0;

	for (i = 0; i < 2; i++) {
		char *got = encrypt_pass(cases[i].in);
		int ok = (strcmp(got, cases[i].expect) == 0);
		printf("case %d [%s]: %s\n", i + 1, cases[i].in,
		       ok ? "MATCH" : "MISMATCH");
		if (!ok) {
			printf("  got : %s\n  want: %s\n", got, cases[i].expect);
			fail = 1;
		}
		free(got);
	}
	return fail;
}
