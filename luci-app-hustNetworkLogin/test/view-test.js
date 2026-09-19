#!/usr/bin/env node
/*
 * 视图逻辑回归测试：用 node 桩掉 LuCI 的模块环境（form/poll/rpc/uci/ui/view/L/E/_/document），
 * 但两处必须**忠实照搬真实行为**，否则这类 bug 测不出来：
 *
 *   1. rpc.declare 的 expect 处理（照搬 luci-static/resources/rpc.js）——
 *      视图拿到的到底是「回复对象」还是被拆开的某个字段，全由那一小段决定。
 *   2. Map.save()/reset() 结尾必定调 Map.renderContents()，它把 map 元素内部
 *      清空后重画（照搬 luci-base form.js:669-711 的 dom.content(mapEl, null)）——
 *      插在 map 元素内部的节点会被冲掉。测试用例 10 就守着这条。
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
 *   7. 点「重连」成功 → 弹 info 提示并解析出 action（回归点：expect 写错会变成 unknown error）
 *   8. 守护进程回 result:false + error → 把它的原话显示出来
 *   9. 配置齐全但服务没跑 → 提示 restart，不误报缺项
 *  10. 保存/重置导致 map 内部 DOM 重建后 → 状态区与按钮仍在页面上，且能继续刷新
 *      （回归点：状态区若插在 map 内部，会被 Map.renderContents() 一起清掉）
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

/* ---- 极简假 DOM：够用就行（append / 清空 / 按 id 或 class 查找）---- */
function append(parent, child) {
	child.parentNode = parent;
	parent.children.push(child);
	return child;
}

function wipe(node) {
	for (const c of node.children)
		c.parentNode = null;

	node.children = [];
	node.textContent = '';
}

function find_by_id(node, id) {
	if (!node)
		return null;

	if (node.id === id)
		return node;

	for (const c of node.children) {
		const hit = find_by_id(c, id);
		if (hit)
			return hit;
	}

	return null;
}

function find_by_class(node, cls) {
	if (!node)
		return null;

	if ((' ' + node.class + ' ').indexOf(' ' + cls + ' ') > -1)
		return node;

	for (const c of node.children) {
		const hit = find_by_class(c, cls);
		if (hit)
			return hit;
	}

	return null;
}

let next_id = 0;
function E(tag, attrs, children) {
	const a = (attrs && !Array.isArray(attrs)) ? attrs : {};
	const node = {
		tag: tag,
		id: a.id || null,
		class: a.class || '',
		attrs: a,
		children: [],
		parentNode: null,
		textContent: '',
		disabled: false,
		keys: next_id++,
		/* LuCI 的 DOM 辅助方法，视图片段里可能用到 */
		appendChild: function(c) { return append(this, c); },
		insertBefore: function(c, ref) {
			const i = ref ? this.children.indexOf(ref) : -1;
			c.parentNode = this;
			if (i < 0)
				this.children.push(c);
			else
				this.children.splice(i, 0, c);

			return c;
		},
		querySelector: function(sel) { return sel.charAt(0) == '.' ? find_by_class(this, sel.slice(1)) : null; }
	};

	const kids = Array.isArray(attrs) ? attrs : (children || []);

	for (const k of kids) {
		if (k == null)
			continue;

		if (typeof k == 'string')
			node.textContent += k;
		else
			append(node, k);
	}

	return node;
}

let doc_root = null;
const document = { getElementById: (id) => find_by_id(doc_root, id) };

/* ---- 其它桩 ---- */
String.prototype.format = function(...a) {
	return this.replace(/%s/g, () => a.shift());
};

let cfg = {};
let daemon = 'ok';           /* ok / missing / refused / last_error */
let last_error = '';
let recon_calls = 0;
let notifications = [];
const maps = [];             /* 视图创建过的 Map 实例，用例 10 用它们模拟 LuCI 的重建 */

const OBJECT_MISSING = new Error('RPCError: RPC call to hust-network-login/status failed with error -32000: Object not found at ClassConstructor.handleCallReply');

/* 表单桩：Map.render() 产出 map 元素（h2 + 一个 section）；
 * Map.renderContents() 照搬 LuCI —— 清空 map 元素内部再重画。 */
const form = {
	Map: function(config, title) {
		this.config = config;
		this.title = title;
		this.el = null;
		maps.push(this);
	},
	NamedSection: function() {},
	Flag: function() {},
	Value: function() {}
};
form.Map.prototype.section = function() {
	return {
		anonymous: false,
		options: [],
		option: function() { const o = {}; this.options.push(o); return o; }
	};
};
form.Map.prototype.renderContents = function() {
	if (this.el)
		wipe(this.el);
	else
		this.el = E('div', { 'id': 'cbi-' + this.config, 'class': 'cbi-map' });

	append(this.el, E('h2', [ this.title || '' ]));
	append(this.el, E('div', { 'class': 'cbi-section' }));

	return Promise.resolve(this.el);
};
form.Map.prototype.render = function() {
	return this.renderContents();
};

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
	addNotification(title, node, kind) { notifications.push({ title, text: node.textContent, kind }); },
	createHandlerFn(o, m) { return o[m].bind(o); }
};
const view = { extend: (o) => o };
const L = { bind: (fn, self) => fn.bind(self) };
const _ = (s) => s;

const v = new Function('form', 'poll', 'rpc', 'uci', 'ui', 'view', 'L', 'E', '_', 'document', src)(
	form, poll, rpc, uci, ui, view, L, E, _, document);

const state = () => { const n = document.getElementById('hust-status-state'); return n ? n.textContent : null; };
const err = () => { const n = document.getElementById('hust-status-error'); return n ? n.textContent : null; };
const btn = () => document.getElementById('hust-reconnect');
const btn_disabled = () => { const b = btn(); return b ? b.disabled : null; };

(async () => {
	/* 先渲染一次页面（真实流程就是这样：render() 之后才有这些 DOM 节点） */
	doc_root = await v.render();
	assert.ok(state() !== null && btn() !== null, '渲染后应有状态区与重连按钮');

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
	await v.handleReconnect({ currentTarget: btn() });
	assert.strictEqual(recon_calls, 0, '缺项时不应该调用 reconnect');
	assert.strictEqual(notifications.length, 1);
	assert.strictEqual(notifications[0].kind, 'warning');
	assert.match(notifications[0].text, /^Missing required settings: Password/);
	console.log('ok  6. 缺项时点重连：不发 RPC，只提示缺什么');

	/* 7. 配置齐全 + 重连成功：解析守护进程回复，弹 info 提示（不能变成 unknown error） */
	cfg.password = 'x';
	notifications = []; recon_calls = 0;
	await v.handleReconnect({ currentTarget: btn() });
	assert.strictEqual(recon_calls, 1);
	assert.strictEqual(notifications.length, 1, '成功应有一条提示');
	assert.strictEqual(notifications[0].kind, 'info', '成功应是 info，实际：' + notifications[0].kind + ' / ' + notifications[0].text);
	assert.doesNotMatch(notifications[0].text, /unknown error/);
	assert.match(notifications[0].text, /signal/);
	assert.strictEqual(btn_disabled(), false);
	console.log('ok  7. 重连成功：info 提示 = ' + notifications[0].text);

	/* 8. 守护进程吞了请求（result:false + error）：显示它的原话 */
	daemon = 'refused';
	notifications = [];
	await v.handleReconnect({ currentTarget: btn() });
	assert.strictEqual(notifications[0].kind, 'warning');
	assert.match(notifications[0].text, /Operation not permitted/);
	console.log('ok  8. 守护进程拒绝：显示 error = ' + notifications[0].text);

	/* 9. 配置齐全但服务没跑：提示 restart，不误报缺项 */
	daemon = 'missing';
	await v.refresh_status();
	assert.match(err(), /no ubus object/);
	assert.doesNotMatch(err(), /Missing required settings/);
	console.log('ok  9. 配置齐全但服务没跑：提示 restart，不误报缺项');

	/* 10. 保存/重置后 map 内部 DOM 重建：状态区与按钮必须还在，且还能刷新。
	 *     （Map.save()/reset() 结尾都会调 renderContents()，它清空 map 元素内部） */
	assert.ok(maps.length > 0, '应能拿到视图创建的 Map 实例');
	maps[0].renderContents();        /* = 点「保存并应用」/「重置」时 LuCI 干的事 */

	assert.ok(document.getElementById('hust-status-state'), '重建后状态区不应消失（要放在 map 元素外面）');
	assert.ok(document.getElementById('hust-status-error'), '重建后「最近错误」行不应消失');
	assert.ok(document.getElementById('hust-reconnect'), '重建后重连按钮不应消失');

	cfg = { enabled: '1', username: 'M2020123123', password: 'x' };
	daemon = 'ok';
	await v.refresh_status();
	assert.strictEqual(state(), 'Online', '重建后状态区还应能被刷新');
	console.log('ok 10. 保存/重置重建 map 内部 DOM 后：状态区与按钮仍在，且能继续刷新');

	console.log('\n视图逻辑全部通过');
})().catch((e) => {
	console.error('FAIL: ' + e.message);
	process.exit(1);
});
