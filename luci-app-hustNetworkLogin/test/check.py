#!/usr/bin/env python3
"""
一致性检查（本地与 CI 都能跑，只用标准库）：

  1. test_url / check_interval 的默认值必须在三处逐字符一致：
       守护进程内置默认  package/hustNetworkLogin/src/main.c
       出厂配置默认      package/hustNetworkLogin/files/etc/config/hust-network-login
       LuCI 表单默认     luci-app-hustNetworkLogin/htdocs/.../view/hustNetworkLogin.js
  2. LuCI 视图里的每个 _('...') 文案都要在 po 里有译文，po 里也不能有失效条目
  3. 所有 JSON 文件可解析；视图能通过 node --check（有 node 时）

用法： python3 luci-app-hustNetworkLogin/test/check.py
"""
import glob
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PKG = os.path.dirname(HERE)                 # luci-app-hustNetworkLogin
REPO = os.path.dirname(PKG)                 # 仓库根

VIEW = os.path.join(PKG, 'htdocs/luci-static/resources/view/hustNetworkLogin.js')
PO = os.path.join(PKG, 'po/zh_Hans/luci-app-hustNetworkLogin.po')
POT = os.path.join(PKG, 'po/templates/luci-app-hustNetworkLogin.pot')
MENU = os.path.join(PKG, 'root/usr/share/luci/menu.d/luci-app-hustNetworkLogin.json')
CONF = os.path.join(REPO, 'package/hustNetworkLogin/files/etc/config/hust-network-login')
MAIN = os.path.join(REPO, 'package/hustNetworkLogin/src/main.c')

fails = 0


def check(ok, msg):
    global fails
    print(('  ok   ' if ok else '  FAIL ') + msg)
    if not ok:
        fails += 1


def js_option_default(js, option):
    """取视图里 o.default = '...' (+ '...') 拼接后的值，option 形如 'test_url'"""
    m = re.search(r"form\.Value, '%s'.*?o\.default\s*=\s*([^;]+);" % option, js, re.S)
    if not m:
        return None
    return re.sub(r'\s+', '', ''.join(re.findall(r"'([^']*)'", m.group(1))))


def c_default():
    block = re.search(r'static const char \*test_url\s*=(.*?);', open(MAIN, encoding='utf-8').read(), re.S)
    return re.sub(r'\s+', '', ''.join(re.findall(r'"([^"]*)"', block.group(1)))) if block else None


print('== 1. 默认值三处一致 ==')
js = open(VIEW, encoding='utf-8').read()
conf = open(CONF, encoding='utf-8').read()

js_url = js_option_default(js, 'test_url')
conf_url = (re.search(r"option test_url '([^']+)'", conf) or [None, None])[1]
c_url = c_default()
check(js_url and js_url == conf_url == c_url,
      'test_url: js == uci == main.c  (%d 字符)' % (len(js_url or '')))
if not (js_url == conf_url == c_url):
    print('       js  = %s\n       uci = %s\n       c   = %s' % (js_url, conf_url, c_url))

js_iv = js_option_default(js, 'check_interval')
conf_iv = (re.search(r"option check_interval '([^']+)'", conf) or [None, None])[1]
c_iv = re.search(r'static int check_interval = (\d+);', open(MAIN, encoding='utf-8').read())
c_iv = c_iv.group(1) if c_iv else None
check(js_iv and js_iv == conf_iv == c_iv,
      'check_interval: js == uci == main.c  (%s)' % js_iv)

print('== 2. 翻译覆盖 ==')
po = open(PO, encoding='utf-8').read()
po_ids = {}
for i, t in re.findall(r'^msgid "(.*)"\nmsgstr "(.*)"$', po, re.M):
    if i:
        po_ids[json.loads('"%s"' % i)] = json.loads('"%s"' % t)

title = list(json.load(open(MENU, encoding='utf-8')).values())[0]['title']
code_ids = set(re.findall(r"_\(\s*'((?:[^'\\]|\\.)*)'", js)) | {title}
check(not (code_ids - set(po_ids)), '视图里所有文案都有 po 条目（缺：%s）' % sorted(code_ids - set(po_ids)))
check(not (set(po_ids) - code_ids), 'po 里没有失效条目（多：%s）' % sorted(set(po_ids) - code_ids))
check(all(v.strip() for v in po_ids.values()), '所有译文都非空')
pot_ids = {json.loads('"%s"' % i)
           for i in re.findall(r'^msgid "(.*)"$', open(POT, encoding='utf-8').read(), re.M) if i}
check(pot_ids == set(po_ids), 'pot 与 po 条目一致（pot 在 %s）' % os.path.relpath(POT, REPO))

print('== 3. JSON / JS ==')
for f in sorted(glob.glob(REPO + '/**/*.json', recursive=True)):
    if '/test/work/' in f:
        continue
    try:
        json.load(open(f, encoding='utf-8'))
    except Exception as e:
        check(False, 'JSON 解析失败 %s: %s' % (f, e))
        break
else:
    check(True, '所有 JSON 可解析')

if subprocess.call(['sh', '-c', 'command -v node >/dev/null']) == 0:
    r = subprocess.run(['node', '--check', VIEW], capture_output=True, text=True)
    check(r.returncode == 0, 'node --check 视图' + ('' if r.returncode == 0 else '  ' + r.stderr[:200]))
else:
    print('  skip node 未安装，跳过 JS 语法检查')

print()
print('%d 项失败' % fails)
sys.exit(1 if fails else 0)
