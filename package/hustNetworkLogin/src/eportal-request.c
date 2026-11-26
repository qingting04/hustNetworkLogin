#include "eportal-request.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/bn.h>

/* 深澜 ePortal 的 RSA 公钥（e=10001, n=94dd2a86...，pageInfo 实测） */
#define MODULUS \
	"94dd2a8675fb779e6b9f7103698634cd400f27a154afa67af6166a43fc2641722" \
	"2a79506d34cacc7641946abda1785b7acf9910ad6a0978c91ec84d40b71d2891" \
	"379af19ffb333e7517e390bd26ac312fe940c340466b4a5d4af1d65c3b5944078" \
	"f96a1a51a5a53e4bc302818b7c9f63c4a1b07bd7d874cef1c3d4b2f5eb7871"
#define EXPONENT "10001"

/*
 * RSA 加密：c = BE(password ">" mac) ^ e mod n，输出小写 hex，左补零到 256 字符。
 * 等价于 Rust 的 BigUint::from_bytes_be(...).modpow(65537, n)。
 */
static char *encrypt_pass(const char *password, const char *mac)
{
	char msg[256];
	BIGNUM *e = NULL, *n = NULL, *m = NULL, *c = NULL;
	BN_CTX *ctx = NULL;
	char *hex = NULL, *out = NULL;
	size_t len, zeros, i;

	snprintf(msg, sizeof(msg), "%s>%s", password, mac);

	ctx = BN_CTX_new();
	c = BN_new();
	BN_hex2bn(&e, EXPONENT);
	BN_hex2bn(&n, MODULUS);
	m = BN_bin2bn((const unsigned char *)msg, (int)strlen(msg), NULL);

	BN_mod_exp(c, m, e, n, ctx);

	hex = BN_bn2hex(c); /* 大写，无前导零 */
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

/* URL 编码（等价 Rust urlencoding::encode，大写 hex） */
static char *url_encode(const char *s)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t len = strlen(s), i, o = 0;
	char *out = malloc(len * 3 + 1);

	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out[o++] = (char)c;
		} else {
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 0xF];
		}
	}
	out[o] = '\0';
	return out;
}

int eportal_build_body(char *body, size_t body_size, const char *username,
		       const char *password, const char *mac, const char *query_string)
{
	char *enc = encrypt_pass(password, mac);
	char *qs_enc = url_encode(query_string);
	int len = snprintf(body, body_size,
			   "userId=%s&password=%s&service=&queryString=%s&passwordEncrypt=true",
			   username, enc, qs_enc);

	free(enc);
	free(qs_enc);
	return len < 0 || (size_t)len >= body_size ? -1 : 0;
}
