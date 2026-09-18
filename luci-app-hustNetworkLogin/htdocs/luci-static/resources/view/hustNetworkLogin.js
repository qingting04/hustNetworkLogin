'use strict';
'require form';
'require uci';
'require ui';
'require rpc';
'require view';

/*
 * The login daemon has no ubus control interface and no signal handling, so
 * "reconnect" means restarting the service: /etc/init.d/hust-network-login
 * implements reload_service() as stop + start, and the daemon authenticates
 * again as soon as it starts.
 *
 * The rc.init ubus method (provided by rpcd) is the canonical way to run an
 * init action from LuCI: it validates the script name and only accepts
 * enable/disable/start/stop/restart/reload.
 */
var callInitAction = rpc.declare({
	object: 'rc',
	method: 'init',
	params: [ 'name', 'action' ]
});

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

		return callInitAction('hust-network-login', 'reload').then(function() {
			ui.addNotification(_('Reconnect'),
				E('p', [ _('The login service was restarted and is authenticating again now. Check "logread | grep -i hust" for details.') ]), 'info');
		}).catch(function(e) {
			ui.addNotification(_('Reconnect'),
				E('p', [ _('Reconnect failed: %s').format(String(e)) ]), 'warning');
		}).then(function() {
			btn.disabled = false;
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
			return E('div', {}, [
				mapEl,
				E('div', { 'class': 'cbi-section' }, [
					E('h3', {}, [ _('Service') ]),
					E('button', {
						'class': 'cbi-button cbi-button-action',
						'click': ui.createHandlerFn(this, 'handleReconnect')
					}, [ _('Reconnect') ]),
					E('p', { 'class': 'cbi-section-descr' }, [
						_('Restarts the login service so it authenticates again immediately - useful when the network changed or the account was kicked by another device. Apply the settings first if you just changed them.')
					])
				])
			]);
		}.bind(this));
	}
});
