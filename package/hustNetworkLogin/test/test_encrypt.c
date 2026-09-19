/*
 * 加密自测：验证 src/modexp.h 的实现与「上游 Rust 版 / 原 OpenSSL 版」逐字节一致。
 *
 * 注意现在**不需要 OpenSSL** —— 这也是它能在 CI 的普通 gcc 里直接跑的原因：
 *   cc -Wall -Wextra -Werror -o test_encrypt package/hustNetworkLogin/test/test_encrypt.c && ./test_encrypt
 * 期望输出：6 个 MATCH，退出码 0。
 *
 * 前两组是上游（black-binary/hust-network-login 的 Rust 实现）已知答案向量，也就是原
 * BN_mod_exp 版本验证过的同一批值；后四组覆盖短消息、高位为 1、长度上限、真实
 * 「密码>mac」形态，期望值由 Python 的 pow(m, e, n)（与 BigUint::modpow 同语义）算出。
 * 更全面的随机差分见 test/modexp-diff.py。
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

static int check(const char *label, const unsigned char *msg, size_t len, const char *want)
{
	char got[MODEXP_HEX + 1];
	int ok;

	modexp_pow_hex(got, msg, len, MODULUS, EXPONENT);
	ok = (strcmp(got, want) == 0);

	printf("case %-28s %s\n", label, ok ? "MATCH" : "MISMATCH");
	if (!ok)
		printf("  got : %s\n  want: %s\n", got, want);

	return ok ? 0 : 1;
}

int main(void)
{
	unsigned char high[64], max120[120];
	int fail = 0;

	memset(high, 0xff, sizeof(high));
	high[0] = 0x80;                        /* 最高位为 1，考进位/符号 */
	memset(max120, 0xff, sizeof(max120));  /* 真实长度上限附近 */

	fail += check("upstream #1 (密码>mac)",
		(const unsigned char *)"123>5ae915bf808f82732e98e01f704f00cd", 36,
		"91a0e02175f6a0b22ad23dac0d7f599806bc091f9fee1bfdada0d24d011dcdaed418296b7c0ec560f988d92a7bb25dbf7ff51752d9bc6482a8180e56f7b772079ab59844abaae91e6d1c4660dc872717f9218f89acc9b70bb32891f28bf9d8f173d81b0e36c828deac919783e4e909ad1c22f953947b4a7ed7c90ac18fd95aa2");

	fail += check("upstream #2 (197)",
		(const unsigned char *)"197", 3,
		"0038c3a7a9719b65a89b82f56bdfc62c71f646403e169fbe1a391d8d1468e648e65e833174db7f1fad21e609ebd21432739e8ee7a3758938b4bd1d07390064918cf1763d6853525b761b055ae3dc229b1579eeacb7281ab258f2ea5c27455861503d814adb857000b24267fca4e70cac4e618f6258367367c0e43c2518e032d8");

	fail += check("1234567890",
		(const unsigned char *)"1234567890", 10,
		"0ffab02bc21fefcc7a4b14a0f143344f2fcf6a10d10d409058fd5421d9dc2c74ee428403b013fb4fbfbd0716d92c19027653d40cfc88fd44cf10948cf47754fec936824e5d212e2d851007f028de5f8ff82727af602077c703939b46db4b6578ba0cf76ac5cd178cc77f0e3ef90a7378ac85847e514cb06d2c64a439343e97e6");

	fail += check("0x80 + ff*63（高位为 1）",
		high, sizeof(high),
		"90fc76023224cd0a157de6c61c64ea550a188844fac34e02087df375c9243e4612e641b4f7ce86aa700d7afd072fef2b51861260767814fdd8fff1793a4e7989eab4b298c3e812a4556030a5549ae9f8651a27bf60a486860f85a81ed9162b28eecbea6fbcb33d4d37dea7e8790ed606028c0e17dcaa4ddb13ab64a508488127");

	fail += check("ff*120（长度上限）",
		max120, sizeof(max120),
		"64a8dfaaa743a0e65c9c6f83be3f8bef916fde1ef597476703cb856211568887ede96d9bfbf9b0234fe53de89e810909c75c27c7d866935c52e1e1ee29364ddff86f8b5bb3b6629487f155359759b8ffcfcefa9d896ca8bfa29e1e522d95e50c8655191414e97865dc95d474dc2f6474bb176fac7f7a7c97276e201d34bc0204");

	fail += check("M2020123123>mac（真实形态）",
		(const unsigned char *)"M2020123123>5ae915bf808f82732e98e01f704f00cd", 44,
		"90d1b96190369eb6ee123c3e23531044bd81100dfb5493d90f0f3f1de62077e5cf005f7a0f6dba277a9bcd53b117ff5cd77f5432747af2f189a58afd18f50fd0a1143447af5493cae3260295d9eab9749d431e33ab2edbb90e83977e39179442b6e786d206cf01f7e5bcaf4eca5bda3de94068dfeb3a91871d106db8e1fed79e");

	printf("\n%s\n", fail ? "有失败" : "全部一致");
	return fail ? 1 : 0;
}
