/* 离线测试用的 OpenSSL BIGNUM 桩：RSA 大数运算不需要真算，只要类型和
 * 内存生命周期对得上（这条路径在「判定已在线」的测试里根本不会走到）。 */
#ifndef _STUB_BN_H
#define _STUB_BN_H

typedef struct { int dummy; } BIGNUM;
typedef struct { int dummy; } BN_CTX;

BIGNUM *BN_new(void);
BN_CTX *BN_CTX_new(void);
int BN_hex2bn(BIGNUM **a, const char *str);
BIGNUM *BN_bin2bn(const unsigned char *s, int len, BIGNUM *ret);
int BN_mod_exp(BIGNUM *r, const BIGNUM *a, const BIGNUM *p, const BIGNUM *m, BN_CTX *ctx);
char *BN_bn2hex(const BIGNUM *a);
void BN_free(BIGNUM *a);
void BN_CTX_free(BN_CTX *c);
void OPENSSL_free(void *p);

#endif
