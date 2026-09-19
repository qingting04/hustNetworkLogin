#!/bin/sh
# ubus 控制面端到端测试（离线跑，需要 gcc / cmake / make / curl / pkg-config 以及能用的用户命名空间）
#
# 为什么需要它：ubusd 只允许 uid 0 发布对象，普通用户直接跑 ubusd 时守护进程永远注册
# 不上（上一版就是因此没能在离线环境验证注册）。这个脚本在 `unshare -r`（映射成 uid 0）
# 里跑真实的 ubusd，用桩掉 libcurl/OpenSSL 的方式编译真实的 src/main.c，验证：
#   1. 对象发布成功（ubus list 里有 hust-network-login）
#   2. status / reconnect 返回的字段与 LuCI 视图约定一致（result 必须是布尔）
#   3. ubusd 晚于守护进程启动 → 守护进程自动重试，对象自己出现（自愈）
#   4. ubusd 中途重启 → 自动重连并重新发布对象，调用继续可用（自愈）
#   5. SIGTERM 干净退出：删 pidfile、对象从总线注销
#   6. 控制面状态（unavailable / reconnecting / registered）只进 syslog，不再有状态文件
#
# 用法（在仓库根目录）：
#   package/hustNetworkLogin/test/ubus-e2e.sh
#
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
	name=$1; repo=$2; shift 2
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

	for t in cmake gcc make curl pkg-config; do
		command -v "$t" >/dev/null || { echo "SKIP 缺少 $t"; exit 0; }
	done

	# 用户命名空间可用吗？（ubusd 只让 uid 0 发布对象，用它把当前用户映射成 0）
	if ! unshare -r id >/dev/null 2>&1; then
		echo "SKIP 当前环境不允许 unshare -r（用户命名空间），跳过 ubus 端到端测试"
		exit 0
	fi

	echo "== 构建依赖（json-c / libubox / ubus），缓存在 $WORK =="
	[ -f "$PREFIX/lib/libjson-c.so" ] || fetch_build json-c json-c/json-c
	[ -f "$PREFIX/lib/libubox.so" ]   || fetch_build libubox openwrt/libubox
	[ -f "$PREFIX/sbin/ubusd" ]       || fetch_build ubus openwrt/ubus

	echo "== 用 src/main.c + 桩（libcurl/OpenSSL/syslog）编译守护进程 =="
	gcc -Wall -Wextra -Werror -I"$STUB" -I"$PREFIX/include" -o "$DAEMON" \
		"$MAIN" "$STUB/stubs.c" "$STUB/syslog_stub.c" \
		-L"$PREFIX/lib" -lubus -lubox -ljson-c -pthread -Wl,-rpath,"$PREFIX/lib" \
		|| { echo "FAIL 编译守护进程"; exit 1; }

	echo "== 进 unshare -r -m 跑端到端（真 ubusd + 真守护进程） =="
	exec unshare -r -m sh "$0" --inside
fi

# ---------------------------------------------------------------- 测试阶段
export LD_LIBRARY_PATH="$PREFIX/lib"
UBUS="$PREFIX/bin/ubus"
fails=0
check() { # check <0|1> <描述>
	if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails + 1)); fi
}
has_object() { "$UBUS" list 2>/dev/null | grep -qx hust-network-login; }

mount -t tmpfs tmpfs /var/run 2>/dev/null && mkdir -p /var/run/ubus
check $? "挂载 tmpfs 到 /var/run 并建出 /var/run/ubus（ubusd 默认 socket 目录）"

mkdir -p "$RUN"
rm -f "$RUN/hust-network-login.pid"

echo "-- 1. 守护进程先起，ubusd 还没起：应记为 unavailable 并持续重试"
HUST_NETWORK_LOGIN_USERNAME=testuser HUST_NETWORK_LOGIN_PASSWORD=testpass \
HUST_NETWORK_LOGIN_CHECK_INTERVAL=3 "$DAEMON" >"$WORK/daemon.log" 2>&1 &
DPID=$!
sleep 2

grep -q 'ubus control plane: unavailable' "$WORK/daemon.log"
check $? "ubusd 不在时日志记 control plane: unavailable"

echo "-- 2. 启动 ubusd：守护进程应在重试周期内自己连上并发布对象"
"$PREFIX/sbin/ubusd" >"$WORK/ubusd.log" 2>&1 &
UPID=$!
sleep 7

has_object; check $? "ubus list 出现 hust-network-login"
grep -q 'ubus control plane: registered' "$WORK/daemon.log"
check $? "日志记 control plane: registered"

ST=$("$UBUS" call hust-network-login status 2>/dev/null)
echo "$ST" | grep -q '"state"'
check $? "ubus call status 返回状态字段（$(echo "$ST" | tr -d '\n\t')）"
echo "$ST" | grep -q '"ubus": "registered"'
check $? "status 的 ubus 字段=registered（控制面自检改由 ubus 暴露）"

"$UBUS" call hust-network-login reconnect | grep -q '"result": true'
check $? "ubus call reconnect 返回布尔 result=true（页面 expect 要求布尔）"

echo "-- 3. ubusd 重启：守护进程应自动重连并重新发布对象"
kill "$UPID" 2>/dev/null
sleep 2
"$PREFIX/sbin/ubusd" >"$WORK/ubusd2.log" 2>&1 &
UPID=$!
sleep 7

has_object; check $? "ubusd 重启后对象自己回来了"
"$UBUS" call hust-network-login status | grep -q '"state"'
check $? "重启后 status 调用仍然可用（不超时）"
grep -q 'ubus control plane: reconnecting' "$WORK/daemon.log"
check $? "ubusd 重启时日志记 control plane: reconnecting"
"$UBUS" call hust-network-login status | grep -q '"ubus": "registered"'
check $? "重启后 status 的 ubus 字段回到 registered"

echo "-- 4. SIGTERM 干净退出"
kill -TERM "$DPID"
sleep 2
kill -0 "$DPID" 2>/dev/null
[ $? -ne 0 ]; check $? "进程已退出"
[ ! -f "$RUN/hust-network-login.pid" ]; check $? "退出时删掉 pidfile"
has_object
[ $? -ne 0 ]; check $? "退出后对象从总线注销"

kill "$UPID" 2>/dev/null
echo
echo "$fails 项失败（守护进程日志：$WORK/daemon.log）"
[ "$fails" = 0 ]
