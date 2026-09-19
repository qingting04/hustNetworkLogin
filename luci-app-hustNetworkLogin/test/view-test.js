#!/usr/bin/env node
/*
 * 视图逻辑回归测试：用 node 桩掉 LuCI 的模块环境（form/poll/rpc/uci/ui/view/L/E/_/document），
 * 但 **rpc.declare 的 expect 处理逐字照搬 luci-static/resources/rpc.js** —— 视图读到的
 * 到底是不是「回复对象」，取决于这一小段代码，桩不忠实就测不出这类 bug。
 *
 * 用法: node luci-app-hustNetworkLogin/test/view-test.js [视图路径]
 *
 * 覆盖：
 *   1. 配置齐全 + ubus 正常 → Online / 无错误 / 重连按钮可用
 *   2. ubus 无对象（-32000 Object not found）→ Service not running / 给出 restart 提示 / 按钮禁用
 *   3. 缺项（未启用 / 缺用户名 / 缺密码 / 缺多项）→「最近错误」行给出缺项提醒
 *   4. 缺项优先于守护进程上报的历史错误
 *   5. 缺项但服务仍在线（改了配置还没保存）→ 状态照实显示，错误行仍提醒缺项
 *   6. 缺项时点「重连」→ 只提示缺什么，不发起 RPC
 *   7. 点「重连」成功 → 解析守护进程回复 {result:true, action:"signal", pid:N} 并给 info 提示
 *      （回归点：expect 写错时这里会变成 "unknown error"）
 *   8. 守护进程回 result:false + error → 把它的原话显示出来
 *   9. 配置齐全但服务没跑 → 提示 restart，不误报缺项
 */
'use strict';

const fs = require('fs');
const path = require('path');
const assert = require('assert');

const VIEW = process.argv[2] || path.join(__dirname, '..', 'htdocs', 'luci-static', 'resources', 'view', 'hustNetworkLogin.js');
const src = fs.readFileSync(VIEW, 'utf8');

const type = Object.prototype.toString;

/* ---- rpc.js 的 expect 逻辑（照搬；注意它只看第一个 key 就 break）---- */
function apply_expect(expect, ret) {
	if (expect) {
		for (const key in expect) {
			if (ret != null && key != '')
				ret = ret[key];
			if (ret == null || type.call(ret) != type.call(expect[key]))
				ret = expect[key];
			break;
		}
	}
	return ret;
}

/* ---- 桩 ---- */
const els = {};
function el(id) {
	return els[id] || (els[id] = { id, textContent: '', disabled: false });
}

String.prototype.format = function(...a) {
	return this.replace(/%s/g, () => a.shift());
};

let cfg = {};              /* UCI 配置桩 */
let daemon = 'ok';         /* ok / missing / refused / last_error */
let last_error = '';
let recon_calls = 0;
let notifications = [];

const OBJECT_MISSING = new Error('RPCError: RPC call to hust-network-login/status failed with error -32000: Object not found at ClassConstructor.handleCallReply');

const form = { Map: function() {} };
const poll = { add: function() {} };
const rpc = {
	declare(opts) {
		return function() {
			let reply;

			if (opts.method === 'status') {
				if (daemon === 'missing')
					return Promise.reject(OBJECT_MISSING);

				reply = { state: 'online', last_error: last_error, updated: 1, running: true, pid: 42 };
			}
			else {
				recon_calls++;

				if (daemon === 'missing')
					return Promise.reject(new Error('RPCError: RPC call to hust-network-login/reconnect failed with error -32000: Object not found at ClassConstructor.handleCallReply'));

				reply = (daemon === 'refused')
					? { result: false, action: 'signal', pid: 42, error: 'Operation not permitted' }
					: { result: true, action: 'signal', pid: 42 };
			}

			return Promise.resolve(apply_expect(opts.expect, reply));
		};
	}
};
const uci = {
	get: (pkg, sec, opt) => cfg[opt],
	load: () => Promise.resolve()
};
const ui = {
	addNotification(title, node, kind) { notifications.push({ title, text: node.children[0], kind }); },
	createHandlerFn(o, m) { return o[m].bind(o); }
};
const view = { extend: (o) => o };
const L = { bind: (fn, self) => fn.bind(self) };
const E = (tag, attrs, children) => ({
	tag,
	attrs: Array.isArray(attrs) ? {} : attrs,
	children: Array.isArray(attrs) ? attrs : children
});
const _ = (s) => s;
const document = { getElementById: (id) => els[id] || null };

const v = new Function('form', 'poll', 'rpc', 'uci', 'ui', 'view', 'L', 'E', '_', 'document', src)(
	form, poll, rpc, uci, ui, view, L, E, _, document);

/* 视图 render() 里创建的状态 span / 重连按钮，这里手工"挂"到页面上 */
el('hust-status-state');
el('hust-status-error');
el('hust-reconnect');

const state = () => el('hust-status-state').textContent;
const err = () => el('hust-status-error').textContent;
const btn_disabled = () => el('hust-reconnect').disabled;

(async () => {
	/* 1. 正常 */
	cfg = { enabled: '1', username: 'M2020123123', password: 'x' };
	daemon = 'ok';
	await v.refresh_status();
	assert.strictEqual(state(), 'Online');
	assert.strictEqual(err(), '-');
	assert.strictEqual(btn_disabled(), false);
	console.log('ok  1. 配置齐全 + ubus 正常：Online / 无错误 / 按钮可用');

	/* 2. 守护进程没在跑 */
	daemon = 'missing';
	await v.refresh_status();
	assert.strictEqual(state(), 'Service not running');
	assert.match(err(), /no ubus object/);
	assert.match(err(), /init\.d\/hust-network-login restart/);
	assert.strictEqual(btn_disabled(), true);
	console.log('ok  2. ubus 无对象：Service not running / 给出 restart 提示 / 按钮禁用');

	/* 3a. 缺项：未启用 */
	daemon = 'ok';
	cfg.enabled = '0';
	await v.refresh_status();
	assert.match(err(), /^Missing required settings: Enable automatic login/);
	assert.match(err(), /Save & Apply/);
	console.log('ok  3a. 未启用：最近错误 = ' + err());

	/* 3b. 缺项：缺用户名 */
	cfg.enabled = '1'; cfg.username = '';
	await v.refresh_status();
	assert.match(err(), /^Missing required settings: Username/);
	console.log('ok  3b. 缺用户名：最近错误 = ' + err());

	/* 3c. 缺项：缺密码 */
	cfg.username = 'M2020123123'; cfg.password = '';
	await v.refresh_status();
	assert.match(err(), /^Missing required settings: Password/);
	console.log('ok  3c. 缺密码：最近错误 = ' + err());

	/* 3d. 缺多项：顺序 启用 → 用户名 → 密码 */
	cfg.enabled = '0'; cfg.username = ''; cfg.password = 'x';
	await v.refresh_status();
	assert.match(err(), /Enable automatic login, Username/);
	console.log('ok  3d. 同时缺多项：按「启用 → 用户名 → 密码」顺序列出');

	/* 4. 缺项优先于守护进程上报的历史错误 */
	cfg.enabled = '1'; cfg.username = '';
	daemon = 'last_error'; last_error = 'all probe urls failed';
	await v.refresh_status();
	assert.match(err(), /^Missing required settings: Username/);
	assert.doesNotMatch(err(), /all probe urls failed/);
	console.log('ok  4. 缺项优先：历史错误被缺项提醒盖住（避免误导）');

	/* 5. 缺项但守护进程还在跑 */
	cfg.username = 'M2020123123'; cfg.password = '';
	daemon = 'ok'; last_error = '';
	await v.refresh_status();
	assert.strictEqual(state(), 'Online');
	assert.match(err(), /^Missing required settings: Password/);
	console.log('ok  5. 缺项但服务在线：状态 = Online，错误行仍给出缺项提醒');

	/* 6. 缺项时点重连：只提示，不发起 RPC */
	notifications = []; recon_calls = 0;
	await v.handleReconnect({ currentTarget: el('hust-reconnect') });
	assert.strictEqual(recon_calls, 0, '缺项时不应该调用 reconnect');
	assert.strictEqual(notifications.length, 1);
	assert.strictEqual(notifications[0].kind, 'warning');
	assert.match(notifications[0].text, /^Missing required settings: Password/);
	console.log('ok  6. 缺项时点重连：不发 RPC，只提示缺什么');

	/* 7. 配置齐全 + 重连成功：必须解析守护进程的回复，不能变成 unknown error */
	cfg.password = 'x';
	notifications = []; recon_calls = 0;
	await v.handleReconnect({ currentTarget: el('hust-reconnect') });
	assert.strictEqual(recon_calls, 1);
	assert.strictEqual(notifications.length, 1, '应有一条通知');
	assert.strictEqual(notifications[0].kind, 'info', '成功应是 info，实际：' + notifications[0].kind + ' / ' + notifications[0].text);
	assert.doesNotMatch(notifications[0].text, /unknown error/);
	assert.match(notifications[0].text, /signal/);
	assert.strictEqual(btn_disabled(), false);
	console.log('ok  7. 重连成功：info 提示 = ' + notifications[0].text);

	/* 8. 守护进程吞了请求（result:false + error）：显示它的原话 */
	daemon = 'refused';
	notifications = [];
	await v.handleReconnect({ currentTarget: el('hust-reconnect') });
	assert.strictEqual(notifications[0].kind, 'warning');
	assert.match(notifications[0].text, /Operation not permitted/);
	console.log('ok  8. 守护进程拒绝：显示 error = ' + notifications[0].text);

	/* 9. 配置齐全但服务没跑：提示 restart，不误报缺项 */
	daemon = 'missing';
	notifications = [];
	await v.refresh_status();
	assert.match(err(), /no ubus object/);
	assert.doesNotMatch(err(), /Missing required settings/);
	console.log('ok  9. 配置齐全但服务没跑：提示 restart，不误报缺项');

	console.log('\n视图逻辑全部通过');
})().catch((e) => {
	console.error('FAIL: ' + e.message);
	process.exit(1);
});
