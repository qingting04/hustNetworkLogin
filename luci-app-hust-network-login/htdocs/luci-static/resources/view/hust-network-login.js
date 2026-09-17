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

		m = new form.Map('hust-network-login', _('校园网自动登录'),
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
