#!/usr/bin/env python3
"""随机差分对拍：src/modexp.h（C 实现） vs Python 的 pow(m, e, n)（数学真值）

为什么要有它：自己写的模幂必须证明与原来的 OpenSSL BN_mod_exp / 上游 Rust 的
BigUint::modpow 等价。固定的两组上游向量不够靠（只覆盖一条路径），这里用 300+ 组
随机消息（覆盖长度 1~256 字节、含高位字节、含比模数还大的消息）逐个对拍。
全程只依赖 cc/python3，不需要 OpenSSL、不需要 SDK。

用法： python3 package/hustNetworkLogin/test/modexp-diff.py
"""
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PKG = os.path.dirname(HERE)
MAIN = os.path.join(PKG, 'src', 'main.c')
CLI_C = os.path.join(HERE, 'modexp_cli.c')
RANDOM_CASES = 300


def load_key():
    src = open(MAIN, encoding='utf-8').read()
    blk = re.search(r'#define MODULUS(.*?)\n#define EXPONENT "([0-9a-fA-F]+)"', src, re.S)
    if not blk:
        sys.exit('FAIL 没能从 src/main.c 里取到 MODULUS / EXPONENT')
    return ''.join(re.findall(r'"([0-9a-fA-F]+)"', blk.group(1))), blk.group(2)


def main():
    mod_hex, exp_hex = load_key()
    n, e = int(mod_hex, 16), int(exp_hex, 16)

    cc = shutil.which('cc') or shutil.which('gcc')
    if not cc:
        print('SKIP 没有 cc/gcc，跳过随机差分')
        return 0

    tmp = tempfile.mkdtemp(prefix='modexp-diff-')
    exe = os.path.join(tmp, 'modexp_cli')
    r = subprocess.run([cc, '-O2', '-Wall', '-Wextra', '-Werror', '-o', exe, CLI_C],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print('FAIL 编译 modexp_cli.c 失败：\n' + r.stderr)
        return 1

    random.seed(20260919)
    lengths = [1, 2, 3, 8, 16, 32, 63, 64, 100, 120, 127, 128, 200, 256]
    cases = [bytes(random.randrange(256) for _ in range(ln)) for ln in lengths]
    cases += [bytes(random.randrange(256) for _ in range(random.randint(1, 256)))
              for _ in range(RANDOM_CASES)]
    cases.append(bytes([0xff]) * 256)               # 比模数还大 → 必须先取模
    cases.append(bytes([0x80] + [0] * 127))         # 高位为 1
    cases.append(bytes([0] * 32))                   # 全零

    r = subprocess.run([exe], input='\n'.join(c.hex() for c in cases) + '\n',
                       capture_output=True, text=True)
    if r.returncode != 0:
        print('FAIL modexp_cli 运行失败：' + r.stderr)
        return 1

    got_lines = r.stdout.strip().split('\n')
    if len(got_lines) != len(cases):
        print('FAIL 输出条数 %d != 用例数 %d' % (len(got_lines), len(cases)))
        return 1

    bad = 0
    for msg, got in zip(cases, got_lines):
        want = ('%x' % pow(int.from_bytes(msg, 'big'), e, n)).rjust(256, '0')
        if got != want:
            bad += 1
            if bad <= 3:
                print('MISMATCH len=%d\n  got : %s\n  want: %s' % (len(msg), got, want))

    shutil.rmtree(tmp, ignore_errors=True)
    print('随机差分 %d 组：%s' % (len(cases), '全部一致' if not bad else '%d 组不一致' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
