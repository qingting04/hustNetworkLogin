'use strict';
'require fs';
'require form';
'require poll';
'require rpc';
'require uci';
'require ui';
'require view';

/*
 * 状态来源：
 *   - 连接状态 / 最近错误 / 最后更新：守护进程写的小文件
 *     /tmp/run/hust-network-login.state（state / last_error / updated）
 *   - 服务是否启用 / 运行：rpcd 的 rc.list
 *
 * 重连：守护进程没有 ubus 控制接口也不处理信号，所以「重连」= 重启服务
 * （/etc/init.d/hust-network-login reload 实现为 stop + start，进程起来即重新认证）。
 * rc.init 是 rpcd 提供的标准接口，会校验脚本名并只允许
 * enable/disable/start/stop/restart/reload。
 */
var STATE_FILE = '/tmp/run/hust-network-login.state';
var SERVICE = 'hust-network-login';

var callInitAction = rpc.declare({
	object: 'rc',
	method: 'init',
	params: [ 'name', 'action' ]
});

var callInitList = rpc.declare({
	object: 'rc',
	method: 'list',
	params: [ 'name' ],
	expect: { '': {} }
});

function parse_state(text) {
	var out = {};

	(text || '').split('\n').forEach(function(line) {
		var m = line.match(/^([a-z_]+)=(.*)$/);

		if (m)
			out[m[1]] = m[2];
	});

	return out;
}

function state_label(state) {
	switch (state) {
	case 'idle':   return _('Starting');
	case 'probe':  return _('Checking connection');
	case 'login':  return _('Authenticating');
	case 'online': return _('Online');
	case 'error':  return _('Login failed, retrying');
	}

	return '-';
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

		return callInitAction(SERVICE, 'reload').then(function() {
			ui.addNotification(_('Reconnect'),
				E('p', [ _('The login service was restarted and is authenticating again now. Check "logread | grep -i hust" for details.') ]), 'info');
		}).catch(function(e) {
			ui.addNotification(_('Reconnect'),
				E('p', [ _('Reconnect failed: %s').format(String(e)) ]), 'warning');
		}).then(function() {
			btn.disabled = false;
			return this.refresh_status();
		}.bind(this));
	},

	/* 读取状态文件 + 服务启用/运行状态，刷新状态区 */
	refresh_status: function() {
		return Promise.all([
			fs.read(STATE_FILE).catch(function() { return null; }),
			callInitList(SERVICE).catch(function() { return null; })
		]).then(function(res) {
			var st = parse_state(res[0]);
			var svc = (res[1] || {})[SERVICE] || null;
			var running = svc ? (svc.running == true || svc.running == 1) : null;

			if (svc)
				set_text('hust-status-service',
					(svc.enabled ? _('Enabled') : _('Disabled')) + ' · ' +
					(running ? _('Running') : _('Stopped')));
			else
				set_text('hust-status-service', '-');

			if (st.state)
				set_text('hust-status-state', state_label(st.state));
			else
				set_text('hust-status-state', running === false ? _('Service not running') : '-');

			set_text('hust-status-error', st.last_error || '-');
			set_text('hust-status-updated',
				st.updated ? new Date(parseInt(st.updated, 10) * 1000).toLocaleString() : '-');
		});
	},

	render: function() {
		var m, s, o;

		m = new form.Map('hust-network-login', _('HUST Network Login'),
			_('Srun ePortal authentication for the HUST campus network. The service keeps running in the background and reconnects automatically when the connection drops; saving the configuration restarts it.'));

		s = m.section(form.NamedSection, 'main', 'hust-network-login');
		s.anonymous = true;

		o = s.option(form.Flag, 'enabled', _('Enable automatic login'));
		o.rmempty = false;

		o = s.option(form.Value, 'username', _('Username'));
		o.rmempty = false;

		o = s.option(form.Value, 'password', _('Password'));
		o.rmempty = false;
		o.password = true;

		o = s.option(form.Value, 'test_url', _('Probe URL'));
		o.rmempty = true;
		o.placeholder = _('comma separated, multiple allowed');
		o.description = _('Addresses used to check whether the connection is up. Multiple addresses are supported, separated by commas (tried in order); leave empty to use the built-in default.');

		o = s.option(form.Value, 'check_interval', _('Check interval (seconds)'));
		o.rmempty = true;
		o.datatype = 'uinteger';
		o.placeholder = '15';
		o.description = _('Interval between online checks, 15 seconds when left empty.');

		return m.render().then(function(mapEl) {
			var box = E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, [ _('Service status') ]),
				E('div', { 'class': 'table' }, [
					row(_('Service'), E('span', { 'id': 'hust-status-service' }, [ '-' ])),
					row(_('Connection state'), E('span', { 'id': 'hust-status-state' }, [ '-' ])),
					row(_('Last error'), E('span', { 'id': 'hust-status-error' }, [ '-' ])),
					row(_('Last update'), E('span', { 'id': 'hust-status-updated' }, [ '-' ]))
				]),
				E('div', { 'class': 'cbi-page-actions' }, [
					E('button', {
						'class': 'cbi-button cbi-button-action',
						'click': ui.createHandlerFn(this, 'handleReconnect')
					}, [ _('Reconnect') ])
				]),
				E('p', { 'class': 'cbi-section-descr' }, [
					_('Restarts the login service so it authenticates again immediately - useful when the network changed or the account was kicked by another device. Apply the settings first if you just changed them.'),
					' ',
					_('The status is written by the login service itself and refreshed every few seconds.')
				])
			]);

			this.refresh_status();
			poll.add(L.bind(this.refresh_status, this));

			return E('div', {}, [ mapEl, box ]);
		}.bind(this));
	}
});
