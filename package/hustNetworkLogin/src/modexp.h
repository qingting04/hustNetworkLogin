/*
 * modexp.h —— 只够深澜 ePortal 用的「大端字节串模幂」，零外部依赖
 *
 * 为什么不直接用 OpenSSL 的 BN_mod_exp：整个 main.c 只用到这一个函数，却因此要从源码
 * 编译整套 OpenSSL（libcrypto + libssl），CI 构建为此多花约 8 分钟 —— 这是构建时长里
 * 最重的一块。这里自己实现「1024 bit 模数 + 小指数」的模幂（只有平方-乘法 + 按位长除法），
 * 约 100 行、无内存分配、无第三方库，也就不用再装任何运行时依赖。
 *
 * 与 OpenSSL 的等价性：输出是 c = m^e mod n 的小写 hex、左补零到 256 字符
 * （原实现是 BN_bn2hex 得到大写无前导零的 hex，再补零转小写），
 * 由 test/test_encrypt.c 的上游已知答案向量 + test/modexp-diff.py 的随机差分保证。
 *
 * 关于常数时间：指数是公开常量（10037），只有底数里含密码。这里没有为安全做常数时间
 * 处理（内层循环不因字节为 0 提前退出，避免额外的数据相关分支），原实现的 BN_mod_exp
 * 本身也不是常数时间函数，所以没有引入新的密码学风险。
 */
#ifndef HUST_NETWORK_LOGIN_MODEXP_H
#define HUST_NETWORK_LOGIN_MODEXP_H

#include <stdint.h>
#include <string.h>

#define MODEXP_BYTES 128                     /* 1024 bit 模数 → 128 字节 */
#define MODEXP_HEX   (MODEXP_BYTES * 2)      /* 输出固定 256 个 hex 字符 */
#define MODEXP_DBL   (MODEXP_BYTES * 2)      /* 被约简的中间值：最多 2048 bit */

static int modexp_hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	return -1;
}

static char modexp_hexchar(unsigned v)
{
	return (char)(v < 10 ? ('0' + v) : ('a' + v - 10));
}

/* hex（长度 ≤ 2*MODEXP_BYTES）→ 右对齐的大端字节串；超长只取低位 */
static void modexp_hex2bytes(uint8_t out[MODEXP_BYTES], const char *hex)
{
	size_t len, i, o = MODEXP_BYTES;
	int half = 0, acc = 0;

	memset(out, 0, MODEXP_BYTES);
	if (!hex)
		return;

	len = strlen(hex);
	if (len > MODEXP_DBL * 2) {
		hex += len - MODEXP_DBL * 2;
		len = MODEXP_DBL * 2;
	}

	for (i = len; i-- > 0; ) {
		int v = modexp_hexval(hex[i]);

		if (v < 0)
			continue;

		if (!half) {
			acc = v;
			half = 1;
		}
		else if (o) {
			out[--o] = (uint8_t)((v << 4) | acc);   /* 大端：右往左填 */
			half = 0;
		}
	}

	if (half && o)
		out[--o] = (uint8_t)acc;
}

/* out(2048b) = a(1024b) * b(1024b)：学童乘法 */
static void modexp_mul(uint8_t out[MODEXP_DBL], const uint8_t a[MODEXP_BYTES],
		       const uint8_t b[MODEXP_BYTES])
{
	size_t i, j;

	memset(out, 0, MODEXP_DBL);

	for (i = 0; i < MODEXP_BYTES; i++) {
		unsigned ai = a[MODEXP_BYTES - 1 - i];   /* a 从最低字节开始 */
		unsigned carry = 0;

		/* 不因 ai == 0 提前退出：避免「密码字节是否为零」这种数据相关分支 */
		for (j = 0; j < MODEXP_BYTES; j++) {
			size_t pos = i + j;              /* ≤ 254，一定落在 256 字节内 */
			unsigned v = out[MODEXP_DBL - 1 - pos] + ai * b[MODEXP_BYTES - 1 - j] + carry;

			out[MODEXP_DBL - 1 - pos] = (uint8_t)v;
			carry = v >> 8;
		}

		for (j = i + MODEXP_BYTES; carry && j < MODEXP_DBL; j++) {  /* 进位继续往上加（至多几步） */
			unsigned v = out[MODEXP_DBL - 1 - j] + carry;

			out[MODEXP_DBL - 1 - j] = (uint8_t)v;
			carry = v >> 8;
		}
	}
}

/* out(1024b) = val(2048b) mod mod(1024b)：按位长除法（每移一位、够减就减） */
static void modexp_mod(uint8_t out[MODEXP_BYTES], const uint8_t val[MODEXP_DBL],
		       const uint8_t mod[MODEXP_BYTES])
{
	uint8_t r[MODEXP_BYTES + 1];    /* 多留一个字节放移出的最高位 */
	uint8_t m2[MODEXP_BYTES + 1];
	size_t i, bit;

	memset(r, 0, sizeof(r));
	memset(m2, 0, sizeof(m2));
	memcpy(m2 + 1, mod, MODEXP_BYTES);

	for (bit = 0; bit < (size_t)MODEXP_DBL * 8; bit++) {
		unsigned carry = (val[bit >> 3] >> (7 - (bit & 7))) & 1u;

		for (i = sizeof(r); i-- > 0; ) {         /* r <<= 1, 低位先进位 */
			unsigned v = ((unsigned)r[i] << 1) | carry;

			r[i] = (uint8_t)v;
			carry = v >> 8;
		}

		if (memcmp(r, m2, sizeof(r)) >= 0) {     /* r >= mod：减一次（r < 2*mod 保证够） */
			unsigned borrow = 0;

			for (i = sizeof(r); i-- > 0; ) {
				int d = (int)r[i] - (int)m2[i] - (int)borrow;

				borrow = (d < 0);
				r[i] = (uint8_t)(d & 0xff);
			}
		}
	}

	memcpy(out, r + 1, MODEXP_BYTES);            /* r < mod < 2^1024 → 最高字节必为 0 */
}

/* out = base^exp mod mod（平方-乘法，指数按大端字节串从高位开始扫） */
static void modexp_pow(uint8_t out[MODEXP_BYTES], const uint8_t base[MODEXP_BYTES],
		       const uint8_t mod[MODEXP_BYTES], const uint8_t *exp, size_t exp_len)
{
	uint8_t res[MODEXP_BYTES], tmp[MODEXP_DBL];
	size_t i, bit;
	int started = 0;

	memset(res, 0, sizeof(res));
	res[MODEXP_BYTES - 1] = 1;                   /* res = 1 */

	for (i = 0; i < exp_len; i++) {
		for (bit = 0; bit < 8; bit++) {
			unsigned b = (exp[i] >> (7 - bit)) & 1u;

			if (!started) {                  /* 跳过指数的高位 0 */
				if (!b)
					continue;

				started = 1;
			}

			modexp_mul(tmp, res, res);       /* res = res^2 */
			modexp_mod(res, tmp, mod);

			if (b) {                         /* res = res * base */
				modexp_mul(tmp, res, base);
				modexp_mod(res, tmp, mod);
			}
		}
	}

	memcpy(out, res, MODEXP_BYTES);
}

/*
 * 公开入口：计算 c = m^e mod n，把结果写成小写 hex、左补零到 MODEXP_HEX 个字符。
 * out 至少 MODEXP_HEX + 1 字节；mod_hex / exp_hex 是任意大小写的 hex 字符串。
 * 消息按大端整数解释并先对 n 取模（与 Rust 版 BigUint::modpow 语义一致，
 * 所以即使消息长度超过模数也不会算错）。
 */
static void modexp_pow_hex(char *out, const uint8_t *msg, size_t msg_len,
			   const char *mod_hex, const char *exp_hex)
{
	uint8_t mod[MODEXP_BYTES], base[MODEXP_BYTES], val[MODEXP_DBL], exp[MODEXP_DBL];
	uint8_t res[MODEXP_BYTES];
	char hex[MODEXP_DBL * 2 + 1];
	size_t i, n = 0, start = MODEXP_BYTES;

	modexp_hex2bytes(mod, mod_hex);
	memset(exp, 0, sizeof(exp));
	modexp_hex2bytes(exp + MODEXP_BYTES, exp_hex);   /* 指数放在低位那 128 字节里 */

	memset(val, 0, sizeof(val));
	if (msg_len > MODEXP_DBL) {
		msg += msg_len - MODEXP_DBL;
		msg_len = MODEXP_DBL;
	}
	memcpy(val + (MODEXP_DBL - msg_len), msg, msg_len);

	modexp_mod(base, val, mod);                      /* base = msg mod n */
	modexp_pow(res, base, mod, exp, sizeof(exp));

	for (i = 0; i < MODEXP_BYTES; i++) {
		if (res[i]) {
			start = i;
			break;
		}
	}

	if (start == MODEXP_BYTES) {                     /* 结果为 0 → 与 BN_bn2hex("0") 一样补零 */
		memset(out, '0', MODEXP_HEX);
		out[MODEXP_HEX] = '\0';
		return;
	}

	for (i = start; i < MODEXP_BYTES; i++) {
		unsigned v = res[i];

		if (!n && (v >> 4) == 0) {               /* 首个字节的高 4 位为 0 → 只写低位 */
			hex[n++] = modexp_hexchar(v & 0xf);
			continue;
		}

		hex[n++] = modexp_hexchar((v >> 4) & 0xf);
		hex[n++] = modexp_hexchar(v & 0xf);
	}

	if (n >= MODEXP_HEX)                             /* 理论到不了（c < n < 2^1024） */
		memcpy(out, hex + (n - MODEXP_HEX), MODEXP_HEX);
	else {
		memset(out, '0', MODEXP_HEX - n);
		memcpy(out + (MODEXP_HEX - n), hex, n);
	}

	out[MODEXP_HEX] = '\0';
}

#endif /* HUST_NETWORK_LOGIN_MODEXP_H */
