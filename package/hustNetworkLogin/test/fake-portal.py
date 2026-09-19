#!/usr/bin/env python3
"""测试用的假门户 / 探测服务器（只依赖 Python 标准库）

给 package/hustNetworkLogin/test/ubus-e2e.sh 用，让 e2e 能在 CI 里跑通**真实的**
HTTP 路径（不再是桩掉 libcurl）：

  GET  /generate_204        → 204 空响应（模拟"已在线"，页面里没有门户标志）
  GET  /portal              → 带门户标志的页面：daemon 从中解析 portal_ip / mac / queryString
  GET  /hang                → 接受连接但永不响应（用来考超时与中断）
  POST /ePortal/InterFace.do?method=login
                            → 校验登录表单，并把「收到的密文」与「Python 算出的期望密文」
                              比对结果写进报告

报告写到 <work>/portal-report.json：
  {"probe_hits": [...], "login_count": N, "user": ..., "cipher_len": N,
   "cipher_ok": true/false, "expected": ..., "got": ..., "query_string": ..., "errors": [...]}

期望密文用 Python 的 pow(m, e, n) 算（与 Rust BigUint::modpow 同语义），
MODULUS / EXPONENT 直接从 src/main.c 里解析，避免两边写两份常量。

用法: python3 fake-portal.py --port 18081 --work /tmp/xxx [--user testuser] [--password testpass]
                              [--mac aa:bb:cc:dd:ee:ff]
"""
import argparse
import json
import os
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN_C = os.path.join(os.path.dirname(HERE), 'src', 'main.c')

ARGS = argparse.Namespace(port=0, work='/tmp', user='', password='', mac='')
REPORT = {'probe_hits': [], 'login_count': 0, 'errors': [], 'requests': []}
LOCK = threading.Lock()


def save_report():
    """每处理完一个请求就落盘：e2e 最后会直接 kill 掉服务器，别指望退出时还能写。"""
    with open(os.path.join(ARGS.work, 'portal-report.json'), 'w') as f:
        json.dump(REPORT, f, ensure_ascii=False, indent=1)


def load_key():
    src = open(MAIN_C, encoding='utf-8').read()
    blk = re.search(r'#define MODULUS(.*?)\n#define EXPONENT "([0-9a-fA-F]+)"', src, re.S)
    if not blk:
        raise SystemExit('FAIL 没能从 src/main.c 里取到 MODULUS / EXPONENT')
    return ''.join(re.findall(r'"([0-9a-fA-F]+)"', blk.group(1))), blk.group(2)


MOD_HEX, EXP_HEX = load_key()


def cipher(password: str, mac: str) -> str:
    msg = ('%s>%s' % (password, mac)).encode()
    e, n = int(EXP_HEX, 16), int(MOD_HEX, 16)
    return ('%x' % pow(int.from_bytes(msg, 'big'), e, n)).rjust(256, '0')


def portal_page(port, mac):
    """门户标志的形态照抄真实 ePortal 跳转页（daemon 用 extract() 按这些标记取值）：
       portal_ip  = http:// 与 /eportal/index.jsp 之间
       mac        = mac= 与 &t= 之间
       query_string = /eportal/index.jsp? 与 '</script>\\r\\n 之间"""
    qs = 'wlanuserip=10.0.0.2&wlanacname=test-ac&mac=%s&t=1700000000&url=http%%3A%%2F%%2Fwww.baidu.com' % mac
    return ("<html><head><script>top.self.location.href='http://127.0.0.1:%d/eportal/index.jsp?%s'"
            "</script>\r\n</head><body>portal</body></html>" % (port, qs))


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    server_version = 'fake-portal/1.0'

    def log_message(self, format, *args):      # noqa: A002 - 与基类签名保持一致
        pass

    def _send(self, code, body=b'', ctype='text/html; charset=UTF-8'):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):
        with LOCK:
            REPORT['probe_hits'].append(self.path)
            REPORT['requests'].append('GET ' + self.requestline)
            save_report()

        if self.path.startswith('/generate_204'):
            self._send(204)
        elif self.path.startswith('/portal'):
            # 登录成功后门户就"放行"了：再探测就是 204（真实校园网也是这个行为），
            # 这样 e2e 里 state 最终会稳定停在 online，而不是反复重新登录
            if REPORT.get('cipher_ok'):
                self._send(204)
            else:
                self._send(200, portal_page(ARGS.port, ARGS.mac).encode())
        elif self.path.startswith('/hang'):
            time.sleep(30)                     # 永不响应：让 daemon 这边走超时/中断分支
            self._send(200, b'too late')
        else:
            self._send(404, b'not found')

    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0)
        body = self.rfile.read(n).decode('utf-8', 'replace')

        with LOCK:
            REPORT['requests'].append('POST ' + self.requestline)

        # 先收下所有 POST（把路径记进报告），别因为路径不匹配就丢掉现场
        if not self.path.startswith('/ePortal/InterFace.do'):
            REPORT['path_mismatch'] = self.path
            save_report()

        want = cipher(ARGS.password, ARGS.mac)
        m = re.search(r'password=([0-9a-fA-F]{256})', body)
        got = m.group(1).lower() if m else ''

        with LOCK:
            REPORT['login_count'] += 1
            REPORT['user'] = (re.search(r'userId=([^&]*)', body) or [None, ''])[1]
            REPORT['query_string'] = (re.search(r'queryString=([^&]*)', body) or [None, ''])[1]
            REPORT['body'] = body
            REPORT['cipher_len'] = len(got)
            REPORT['expected'] = want
            REPORT['got'] = got
            REPORT['cipher_ok'] = (got == want)
            REPORT['password_encrypt'] = 'passwordEncrypt=true' in body
            save_report()

        self._send(200, b'{"result":"success","message":"ok"}', 'application/json; charset=UTF-8')


def main():
    global ARGS

    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--work', required=True)
    ap.add_argument('--user', default='testuser')
    ap.add_argument('--password', default='testpass')
    ap.add_argument('--mac', default='aa:bb:cc:dd:ee:ff')
    ARGS = ap.parse_args()

    srv = ThreadingHTTPServer(('127.0.0.1', ARGS.port), Handler)
    srv.daemon_threads = True

    with open(os.path.join(ARGS.work, 'portal-port'), 'w') as f:
        f.write(str(ARGS.port))

    srv.serve_forever()


if __name__ == '__main__':
    main()
