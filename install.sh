#!/bin/sh
# ============================================================================
# hust-network-login + LuCI（JS 版）手动安装脚本（在路由器上以 root 运行）
#
# 用法：
#   1) 把 hust-network-login 二进制 和 本脚本 一起传到路由器（scp 到 /tmp/）
#   2) sh /tmp/install.sh /tmp/hust-network-login
#
# 完成后在 LuCI 网页「服务 → 校园网登录」里配置账号密码并启用。
# ============================================================================

BIN_SRC="${1:-/tmp/hust-network-login}"

[ "$(id -u)" = "0" ] || { echo "请以 root 运行"; exit 1; }

echo "==> 安装二进制"
if [ -f "$BIN_SRC" ]; then
	install -m 0755 "$BIN_SRC" /usr/bin/hust-network-login
	echo "    二进制已安装: /usr/bin/hust-network-login"
else
	echo "!! 未找到二进制 $BIN_SRC"
	echo "   请先 scp 二进制到路由器，例如:"
	echo "   scp hust-network-login root@192.168.1.1:/tmp/"
	exit 1
fi

echo "==> 写 init.d 服务脚本"
cat > /etc/init.d/hust-network-login <<'EOF'
#!/bin/sh /etc/rc.common
USE_PROCD=1
START=90
STOP=10

PROG="/usr/bin/hust-network-login"
CONFIG="hust-network-login"

start_service() {
	local enabled username password
	config_load "$CONFIG"
	config_get_bool enabled "main" "enabled" "0"
	config_get username "main" "username"
	config_get password "main" "password"

	[ "$enabled" = "1" ] || return 0
	[ -n "$username" ] || return 0
	[ -n "$password" ] || return 0
	[ -x "$PROG" ] || return 0

	procd_open_instance
	procd_set_param command "$PROG"
	procd_set_param env HUST_NETWORK_LOGIN_USERNAME="$username" \
	                     HUST_NETWORK_LOGIN_PASSWORD="$password"
	procd_set_param respawn 3600 5 5
	procd_set_param file "/etc/config/$CONFIG"
	procd_close_instance
}

service_triggers() {
	procd_add_reload_trigger "$CONFIG"
}

reload_service() {
	stop
	start
}
EOF
chmod 0755 /etc/init.d/hust-network-login

echo "==> 写 UCI 默认配置（若已存在则跳过）"
if [ ! -f /etc/config/hust-network-login ]; then
	cat > /etc/config/hust-network-login <<'EOF'
config main 'main'
	option enabled '0'
	option username ''
	option password ''
EOF
fi

echo "==> 写 LuCI 菜单（JS 版）"
mkdir -p /usr/share/luci/menu.d
cat > /usr/share/luci/menu.d/luci-app-hustNetworkLogin.json <<'EOF'
{
	"admin/services/hustNetworkLogin": {
		"title": "校园网登录",
		"order": 90,
		"action": {
			"type": "view",
			"path": "hustNetworkLogin"
		}
	}
}
EOF

echo "==> 写 LuCI 视图（JS 版）"
mkdir -p /www/luci-static/resources/view
cat > /www/luci-static/resources/view/hustNetworkLogin.js <<'EOF'
'use strict';
'require form';
'require uci';
'require view';

return view.extend({
	load: function() {
		return uci.load('hust-network-login');
	},

	render: function() {
		var m, s, o;

		m = new form.Map('hust-network-login', _('hustNetworkLogin'),
			_('深澜 ePortal 认证（华中科技大学校园网）。开启后服务常驻运行，掉线后 15 秒自动重连；保存配置会自动重启服务。'));

		s = m.section(form.NamedSection, 'main', 'hust-network-login');
		s.anonymous = true;

		o = s.option(form.Flag, 'enabled', _('启用自动登录'));
		o.rmempty = false;

		o = s.option(form.Value, 'username', _('用户名'));
		o.rmempty = false;
		o.placeholder = 'M202674581';

		o = s.option(form.Value, 'password', _('密码'));
		o.rmempty = false;
		o.password = true;

		return m.render();
	}
});
EOF

echo "==> 清理 LuCI 缓存"
rm -rf /tmp/luci-modulecache /tmp/luci-indexcache

echo "==> 启用服务"
/etc/init.d/hust-network-login enable

echo ""
echo "安装完成！"
echo "  - 在 LuCI 网页「服务 → 校园网登录」填入用户名/密码，勾选「启用」并保存。"
echo "  - 命令行配置: uci set hust-network-login.main.username='学号'"
echo "               uci set hust-network-login.main.password='密码'"
echo "               uci set hust-network-login.main.enabled='1'"
echo "               uci commit hust-network-login && /etc/init.d/hust-network-login reload"
