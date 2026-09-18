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

		return m.render();
	}
});
