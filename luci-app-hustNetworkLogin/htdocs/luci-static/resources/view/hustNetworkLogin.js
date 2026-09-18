'use strict';
'require form';
'require poll';
'require rpc';
'require uci';
'require ui';
'require view';

/*
 * 状态与控制都走 ubus 对象 hust-network-login（由 rpcd 的 ucode 插件提供）：
 *   status    → { state, last_error, updated, running, pid, enabled }
 *   reconnect → 让守护进程立刻重新认证（SIGHUP，不重启进程）
 * 字段命名与 jluNetworkLogin 的 ubus status 对齐。
 *
 * 页面布局：标题 → 服务状态（含「重连」按钮）→ 设置 → 底部保存/应用。
 */
var callStatus = rpc.declare({
	object: 'hust-network-login',
	method: 'status',
	expect: { '': {} }
});

var callReconnect = rpc.declare({
	object: 'hust-network-login',
	method: 'reconnect',
	expect: { result: false, error: '', action: '' }
});

function state_label(state) {
	switch (state) {
	case 'idle':    return _('Starting');
	case 'probe':   return _('Checking connection');
	case 'login':   return _('Authenticating');
	case 'online':  return _('Online');
	case 'error':   return _('Login failed, retrying');
	case 'stopped': return _('Stopped');
	}

	return _('Unknown');
}

function row(label, node) {
	return E('div', { 'class': 'tr' }, [
		E('div', { 'class': 'td left' }, [ label ]),
		E('div', { 'class': 'td left' }, [ node ])
	]);
}

function set_text(id, text) {
	var el = document.getElementById(id);

	if (el)
		el.textContent = text;
}

return view.extend({
	load: function() {
		return uci.load('hust-network-login');
	},

	handleReconnect: function(ev) {
		var btn = ev.currentTarget;

		if (uci.get('hust-network-login', 'main', 'enabled') != '1') {
			ui.addNotification(_('Reconnect'),
				E('p', [ _('The service is disabled - enable it and apply the settings first.') ]), 'warning');

			return Promise.resolve();
		}

		btn.disabled = true;

		return callReconnect().then(function(res) {
			if (res && res.result) {
				ui.addNotification(_('Reconnect'),
					E('p', [ _('The login service is authenticating again now (requested by %s). Check "logread | grep -i hust" for details.').format(res.action || 'signal') ]), 'info');
			}
			else {
				ui.addNotification(_('Reconnect'),
					E('p', [ _('Reconnect failed: %s').format((res && res.error) || _('unknown error')) ]), 'warning');
			}
		}).catch(function(e) {
			ui.addNotification(_('Reconnect'),
				E('p', [ _('Reconnect failed: %s').format(String(e)) ]), 'warning');
		}).then(function() {
			btn.disabled = false;
			return this.refresh_status();
		}.bind(this));
	},

	refresh_status: function() {
		/* 只显示连接状态与最近错误：守护进程不在运行时 ubus 会返回 state=stopped，
		 * 所以不需要单独一行「服务状态」。 */
		return callStatus().catch(function() { return null; }).then(function(st) {
			set_text('hust-status-state', st ? state_label(st.state) : _('Unknown'));
			set_text('hust-status-error', (st && st.last_error) || '-');
		});
	},

	render: function() {
		var m, s, o;

		m = new form.Map('hust-network-login', _('HUST Network Login'));

		s = m.section(form.NamedSection, 'main', 'hust-network-login');
		s.anonymous = true;

		o = s.option(form.Flag, 'enabled', _('Enable automatic login'));
		o.rmempty = false;

		o = s.option(form.Value, 'username', _('Username'));
		o.rmempty = false;

		o = s.option(form.Value, 'password', _('Password'));
		o.rmempty = false;
		o.password = true;

		/* 默认值必须与守护进程内置默认（src/main.c 的 test_url）和
		 * /etc/config/hust-network-login 的出厂默认保持一致；这里显式给出，
		 * 老配置里没有这一项时输入框也会显示实际使用的地址列表。 */
		o = s.option(form.Value, 'test_url', _('Probe URL'));
		o.default = 'http://connect.rom.miui.com/generate_204,' +
			'http://connectivitycheck.platform.hicloud.com/generate_204,' +
			'http://www.baidu.com';
		o.rmempty = true;
		o.placeholder = _('comma separated, multiple allowed');

		o = s.option(form.Value, 'check_interval', _('Check interval (seconds)'));
		o.default = '15';
		o.rmempty = true;
		o.datatype = 'uinteger';
		o.placeholder = '15';

		return m.render().then(function(mapEl) {
			var box = E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, [ _('Service status') ]),
				E('div', { 'class': 'table' }, [
					row(_('Connection state'), E('span', { 'id': 'hust-status-state' }, [ '-' ])),
					row(_('Last error'), E('span', { 'id': 'hust-status-error' }, [ '-' ]))
				]),
				E('div', { 'class': 'cbi-page-actions' }, [
					E('button', {
						'class': 'cbi-button cbi-button-action',
						'click': ui.createHandlerFn(this, 'handleReconnect')
					}, [ _('Reconnect') ])
				])
			]);

			this.refresh_status();
			poll.add(L.bind(this.refresh_status, this));

			/* 状态区放在页面标题之后、设置表单之前 */
			mapEl.insertBefore(box, mapEl.querySelector('.cbi-section'));

			return mapEl;
		}.bind(this));
	}
});
