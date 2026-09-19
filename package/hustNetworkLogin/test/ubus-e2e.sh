#!/bin/sh
# ubus 控制面 + HTTP 登录路径端到端测试（离线跑，需要 gcc / cmake / make / curl / python3
# 以及可用的用户命名空间）
#
# 为什么要这么测：
#   1) ubusd 只允许 uid 0 发布对象（ubusd_acl_check 里 uid==0 直接放行），普通用户跑
#      ubusd 时守护进程永远注册不上 —— 所以在 `unshare -r`（映射成 uid 0）里跑真 ubusd。
#   2) 守护进程现在自带明文 HTTP 实现（src/http.h，零第三方依赖），所以这里不再拿桩去替
#      掉 libcurl，而是起一个本机假门户（test/fake-portal.py），把**真实**的
#      HTTP 路径跑一遍：探测 → 门户页面解析 → RSA 加密 → 登录 POST，
#      并核对登录表单里的密文与 Python 用 pow(m,e,n) 算出的期望值逐字节一致。
#
# 覆盖：
#   1. 在线探测（假 generate_204）→ state=online；ubus 对象发布；status/reconnect 字段契约
#   2. 假门户登录：解析 portal_ip/mac/queryString → 加密 → POST；密文与期望值一致
#   3. 探测地址全不可达 → state=error、last_error="all probe urls failed"
#   4. ubusd 中途重启 → 自动重连并重新发布对象，status 调用不超时（自愈）
#   5. 卡住的请求能被 SIGHUP（重连）立刻打断
#   6. SIGTERM 干净退出：state=stopped、删 pidfile、对象从总线注销
#
# 用法（仓库根目录）：sh package/hustNetworkLogin/test/ubus-e2e.sh
# 依赖的 3 个上游库（json-c / libubox / ubus）只在第一次下载+编译，缓存在 $WORK 里。
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
PKG=$(dirname "$HERE")
MAIN="$PKG/src/main.c"

WORK=${WORK:-/tmp/hust-ubus-e2e}
PREFIX="$WORK/prefix"
SRC="$WORK/src"
DAEMON="$WORK/hust-network-login"
STUB="$HERE/stubs"
RUN=/tmp/run
STATE="$RUN/hust-network-login.state"

fetch() { # fetch <name> <repo>
	if [ ! -f "$SRC/$1.tar.gz" ]; then
		echo "  下载 $1 ..."
		curl -sSL -o "$SRC/$1.tar.gz" "https://github.com/$2/archive/refs/heads/master.tar.gz" || return 1
	fi
	rm -rf "$SRC/$1-master"
	tar xzf "$SRC/$1.tar.gz" -C "$SRC" || return 1

	return 0
}

fetch_build() { # fetch_build <name> <repo> [extra cmake args...]
	name=$1
	repo=$2
	shift 2

	fetch "$name" "$repo" || { echo "FAIL 下载 $name"; exit 1; }

	cmake -S "$SRC/$name-master" -B "$SRC/$name-master/build" \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
		-DCMAKE_BUILD_TYPE=Release -DBUILD_LUA=OFF -DBUILD_EXAMPLES=OFF \
		-DBUILD_TESTING=OFF "$@" >"$WORK/$name-cmake.log" 2>&1 || {
			echo "FAIL configure $name（见 $WORK/$name-cmake.log）"; exit 1; }
	cmake --build "$SRC/$name-master/build" -j"$(nproc 2>/dev/null || echo 2)" \
		>"$WORK/$name-build.log" 2>&1 || { echo "FAIL build $name（见 $WORK/$name-build.log）"; exit 1; }
	cmake --install "$SRC/$name-master/build" >/dev/null 2>&1 || exit 1
}

# ---------------------------------------------------------------- 构建阶段
if [ "${1:-}" != "--inside" ]; then
	mkdir -p "$SRC" "$PREFIX"

	for t in cmake gcc make curl python3; do
		command -v "$t" >/dev/null || { echo "SKIP 缺少 $t"; exit 0; }
	done

	if ! unshare -r id >/dev/null 2>&1; then
		echo "SKIP 当前环境不允许 unshare -r（用户命名空间），跳过 ubus 端到端测试"
		exit 0
	fi

	echo "== 构建依赖（json-c / libubox / ubus），缓存在 $WORK =="
	[ -f "$PREFIX/lib/libjson-c.so" ] || fetch_build json-c json-c/json-c
	[ -f "$PREFIX/lib/libubox.so" ]   || fetch_build libubox openwrt/libubox
	[ -f "$PREFIX/sbin/ubusd" ]       || fetch_build ubus openwrt/ubus

	echo "== 编译守护进程（真 main.c + 自带 http.h/modexp.h，只桩 syslog） =="
	gcc -Wall -Wextra -Werror -I"$PKG/src" -o "$DAEMON" \
		"$MAIN" "$STUB/syslog_stub.c" \
		-L"$PREFIX/lib" -lubus -lubox -ljson-c -pthread -Wl,-rpath,"$PREFIX/lib" \
		|| { echo "FAIL 编译守护进程"; exit 1; }

	# 假门户的端口：一个给门户，一个保证没人监听（考"全不可达"分支）
	PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
	DEAD=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
	export PORT DEAD
	echo "== 进 unshare -r -m 跑端到端（真 ubusd + 真 HTTP + 假门户 $PORT） =="
	exec unshare -r -m sh "$0" --inside
fi

# ---------------------------------------------------------------- 测试阶段
export LD_LIBRARY_PATH="$PREFIX/lib"
UBUS="$PREFIX/bin/ubus"
LOG="$WORK/daemon.log"
REPORT="$WORK/portal-report.json"
fails=0

check() { # check <0|1> <描述>
	if [ "$1" = 0 ]; then
		echo "  ok   $2"
	else
		echo "  FAIL $2"
		fails=$((fails + 1))
	fi
}

state_field() { sed -n "s/^$1=//p" "$STATE" 2>/dev/null; }

has_object() { "$UBUS" list 2>/dev/null | grep -qx hust-network-login; }

json() { # json <表达式文件> <key>
	python3 -c "
import json,sys
d=json.load(open('$1'))
print(d.get('$2'))
" 2>/dev/null
}

start_daemon() { # start_daemon <test_url>
	rm -f "$STATE"
	HUST_NETWORK_LOGIN_USERNAME=testuser \
	HUST_NETWORK_LOGIN_PASSWORD=testpass \
	HUST_NETWORK_LOGIN_TEST_URL="$1" \
	HUST_NETWORK_LOGIN_CHECK_INTERVAL=3 \
	"$DAEMON" >>"$LOG" 2>&1 &
	DPID=$!
}

stop_daemon() {
	kill -TERM "$DPID" 2>/dev/null
	i=0
	while kill -0 "$DPID" 2>/dev/null && [ "$i" -lt 25 ]; do
		sleep 0.2
		i=$((i + 1))
	done
}

mount -t tmpfs tmpfs /var/run 2>/dev/null && mkdir -p /var/run/ubus
check $? "挂载 tmpfs 到 /var/run 并建出 /var/run/ubus（ubusd 默认 socket 目录）"

mkdir -p "$RUN" "$WORK"
rm -f "$STATE" "$RUN/hust-network-login.pid" "$LOG" "$REPORT"

echo "-- 0. 起假门户与 ubusd"
python3 "$HERE/fake-portal.py" --port "$PORT" --work "$WORK" >"$WORK/portal.log" 2>&1 &
PPORT=$!
"$PREFIX/sbin/ubusd" >"$WORK/ubusd.log" 2>&1 &
UPID=$!
sleep 1

kill -0 "$PPORT" 2>/dev/null
check $? "假门户在 $PORT 上跑起来了"
kill -0 "$UPID" 2>/dev/null
check $? "ubusd 起来了"

echo "-- 1. 在线探测：state=online + ubus 对象与字段契约"
start_daemon "http://127.0.0.1:$PORT/generate_204"
sleep 3

kill -0 "$DPID" 2>/dev/null
check $? "守护进程在跑"
has_object
check $? "ubus list 出现 hust-network-login"
[ "$(state_field ubus)" = "registered" ]
check $? "状态文件写 ubus=registered"
[ "$(state_field state)" = "online" ]
check $? "假 generate_204 → state=online（探测路径走通）"
[ -z "$(state_field last_error)" ]
check $? "在线时 last_error 为空"

ST=$("$UBUS" call hust-network-login status 2>/dev/null)
echo "$ST" | grep -q '"state"'
check $? "ubus call status 返回状态字段（$(echo "$ST" | tr -d '\n\t')）"

"$UBUS" call hust-network-login reconnect | grep -q '"result": true'
check $? "ubus call reconnect 返回布尔 result=true（页面 expect 要求布尔）"

echo "-- 2. 假门户登录：解析 → 加密 → POST（密文与 Python 期望值对拍）"
stop_daemon
start_daemon "http://127.0.0.1:$PORT/portal"
sleep 3

mkdir -p "$WORK"
[ -f "$REPORT" ]
check $? "假门户收到了请求并写出报告"
[ "$(json "$REPORT" login_count)" = "1" ]
check $? "登录 POST 恰好发了一次（login_count=$(json "$REPORT" login_count)）"
[ "$(json "$REPORT" cipher_ok)" = "True" ]
check $? "登录密文与 Python pow(m,e,n) 期望值一致（cipher=256 hex? $(json "$REPORT" cipher_len) 位）"
[ "$(json "$REPORT" password_encrypt)" = "True" ]
check $? "表单带 passwordEncrypt=true"
[ "$(json "$REPORT" user)" = "testuser" ]
check $? "表单 userId 正确（$(json "$REPORT" user)）"
grep -q "/portal" "$REPORT"
check $? "门户页面被真正 GET 过"
[ "$(state_field state)" = "online" ]
check $? "登录成功后状态回到 online"

echo "-- 3. 探测地址全不可达 → state=error"
stop_daemon
start_daemon "http://127.0.0.1:$DEAD/nothing"
sleep 3

[ "$(state_field state)" = "error" ]
check $? "state=error（$(state_field state)）"
case "$(state_field last_error)" in
*"all probe urls failed"*)
	check 0 "last_error = $(state_field last_error)" ;;
*)
	check 1 "last_error 应为 all probe urls failed，实际：$(state_field last_error)" ;;
esac

echo "-- 4. 卡住的请求能被 SIGHUP（重连）立刻打断"
stop_daemon
start_daemon "http://127.0.0.1:$PORT/hang"
sleep 1
# 只看这条命令之后新写进日志的部分（前面的阶段也可能记过同样的话）
off=$(wc -c <"$LOG")
off=$((off + 1))
"$UBUS" call hust-network-login reconnect >/dev/null 2>&1
i=0
hit=1
while [ "$i" -lt 30 ]; do
	if tail -c "+$off" "$LOG" | grep -q "reconnect requested, restarting cycle"; then
		hit=0
		break
	fi
	sleep 0.1
	i=$((i + 1))
done
check $hit "3 秒内打断卡住的请求（worker 立刻进入下一轮；HTTP 总超时是 10 秒）"

echo "-- 5. ubusd 重启 → 自动重连并重新发布对象"
stop_daemon
start_daemon "http://127.0.0.1:$PORT/generate_204"
sleep 2
kill "$UPID" 2>/dev/null
sleep 2
"$PREFIX/sbin/ubusd" >"$WORK/ubusd2.log" 2>&1 &
UPID=$!
sleep 7

has_object
check $? "ubusd 重启后对象自己回来了"
"$UBUS" call hust-network-login status | grep -q '"state"'
check $? "重启后 status 调用仍然可用（不超时）"
[ "$(state_field ubus)" = "registered" ]
check $? "状态文件重新写回 ubus=registered"

echo "-- 6. SIGTERM 干净退出"
stop_daemon
kill -0 "$DPID" 2>/dev/null
[ $? -ne 0 ]
check $? "进程已退出"
[ "$(state_field state)" = "stopped" ]
check $? "退出时写 state=stopped"
[ ! -f "$RUN/hust-network-login.pid" ]
check $? "退出时删掉 pidfile"
has_object
[ $? -ne 0 ]
check $? "退出后对象从总线注销"

kill "$UPID" "$PPORT" 2>/dev/null
echo
echo "$fails 项失败（守护进程日志：$LOG，门户报告：$REPORT）"
[ "$fails" = 0 ]
