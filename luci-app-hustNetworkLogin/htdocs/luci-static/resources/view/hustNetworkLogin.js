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
			_('深澜 ePortal 认证（华中科技大学校园网）。开启后服务常驻运行，掉线自动重连；保存配置会自动重启服务。'));

		s = m.section(form.NamedSection, 'main', 'hust-network-login');
		s.anonymous = true;

		o = s.option(form.Flag, 'enabled', _('启用自动登录'));
		o.rmempty = false;

		o = s.option(form.Value, 'username', _('用户名'));
		o.rmempty = false;

		o = s.option(form.Value, 'password', _('密码'));
		o.rmempty = false;
		o.password = true;

		o = s.option(form.Value, 'test_url', _('探测地址'));
		o.rmempty = true;
		o.placeholder = '多个地址用逗号分隔';
		o.description = _('用于检测是否在线的地址，支持逗号分隔多个（自动 fallback），留空用默认值');

		o = s.option(form.Value, 'check_interval', _('检测间隔（秒）'));
		o.rmempty = true;
		o.datatype = 'uinteger';
		o.placeholder = '15';
		o.description = _('在线时的检测周期，留空默认 15 秒');

		return m.render();
	}
});
