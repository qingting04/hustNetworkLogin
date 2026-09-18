/*
 * hust-network-login (C 版)
 * 华中科技大学校园网（深澜 ePortal）自动登录，掉线 15 秒自动重连。
 *
 * 依赖：libcurl（HTTP）、libcrypto/libopenssl（RSA 大数运算）。
 * 配置：环境变量 HUST_NETWORK_LOGIN_USERNAME / HUST_NETWORK_LOGIN_PASSWORD，
 *       或命令行传配置文件路径（两行：第一行用户名，第二行密码）。
 *
 * 状态上报：把 state / last_error / updated 写入 /tmp/run/hust-network-login.state，
 *           供 LuCI 页面（luci-app-hustNetworkLogin，经 rpcd 的 ubus 对象）读取；
 *           PID 另写 /tmp/run/hust-network-login.pid。
 * 控制：SIGHUP = 中断当前 HTTP 传输并立刻重新认证（重连，不重启进程）；
 *       SIGTERM/SIGINT = 干净退出（写 state=stopped、删 pidfile）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <curl/curl.h>
#include <openssl/bn.h>

/* 在线探测地址：默认用轻量的 captive portal 检测地址（在线时返回 204 空响应），
 * 掉线时同样会被门户拦截并返回带 query string 的页面。
 * 可用环境变量 HUST_NETWORK_LOGIN_TEST_URL 覆盖。 */
static const char *test_url =
	"http://connect.rom.miui.com/generate_204,"
	"http://connectivitycheck.platform.hicloud.com/generate_204,"
	"http://www.baidu.com";
/* 在线检测间隔（秒），可用环境变量 HUST_NETWORK_LOGIN_CHECK_INTERVAL 覆盖 */
static int check_interval = 15;

/*
 * 运行状态上报：把状态与最近一次错误写进一个小文件，供 LuCI 界面轮询显示。
 * 路径要和 luci-app-hustNetworkLogin 的 acl.d 里声明的路径完全一致
 * （rpcd 读文件时会 realpath 后再校验 ACL，所以这里用没有符号链接的 /tmp/run）。
 */
static const char *state_file = "/tmp/run/hust-network-login.state";
static const char *pid_file = "/tmp/run/hust-network-login.pid";
static char last_error[224];

/*
 * 控制标志（只由信号处理函数写入，async-signal-safe）：
 *   g_stop      SIGTERM/SIGINT → 干净退出
 *   g_reconnect SIGHUP         → 中断当前 HTTP 传输并立刻重新认证
 * 于是「重连」不需要重启进程，init 脚本/procd/LuCI 都能秒级触发。
 */
static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_reconnect = 0;

static void sig_handler(int sig)
{
	if (sig == SIGHUP)
		g_reconnect = 1;
	else
		g_stop = 1;
}

/*
 * 记录最近一次错误：既进 syslog，也留给 Web 界面显示。
 * 被手动重连（SIGHUP）打断的传输不算错误，直接忽略。
 */
static void set_error(const char *fmt, ...)
{
	va_list ap;

	if (g_reconnect)
		return;

	va_start(ap, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, ap);
	va_end(ap);

	syslog(LOG_ERR, "%s", last_error);
}

/* 状态：idle 启动中 / probe 探测中 / login 认证中 / online 已在线 / error 登录失败重试中 / stopped 已停止
 * 先写临时文件再 rename，避免界面读到写了一半的内容 */
static void write_state(const char *state)
{
	char tmp[sizeof("/tmp/run/hust-network-login.state.tmp")];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.tmp", state_file);

	f = fopen(tmp, "w");
	if (!f)
		return;

	fprintf(f, "state=%s\n", state);
	fprintf(f, "last_error=%s\n", last_error);
	fprintf(f, "updated=%ld\n", (long)time(NULL));
	fclose(f);

	rename(tmp, state_file);
}

/* procd 之外也让 init 脚本/其它工具能找到 PID（SIGHUP 重连用） */
static void write_pidfile(void)
{
	FILE *f = fopen(pid_file, "w");

	if (!f)
		return;

	fprintf(f, "%ld\n", (long)getpid());
	fclose(f);
}

/* 可被打断的睡眠：被 SIGHUP/SIGTERM 打断就立刻返回 */
static void sleep_interruptible(int seconds)
{
	while (seconds > 0 && !g_stop && !g_reconnect)
		seconds = sleep((unsigned int)seconds);
}

/* libcurl 进度回调：非 0 表示中断传输（手动重连时让正在飞的请求立刻收摊） */
static int xferinfo_cb(void *p, curl_off_t dltotal, curl_off_t dlnow,
		       curl_off_t ultotal, curl_off_t ulnow)
{
	(void)p, (void)dltotal, (void)dlnow, (void)ultotal, (void)ulnow;
	return g_reconnect ? 1 : 0;
}

/* 深澜 ePortal 的 RSA 公钥（e=10001, n=94dd2a86...，pageInfo 实测） */
#define MODULUS \
	"94dd2a8675fb779e6b9f7103698634cd400f27a154afa67af6166a43fc2641722" \
	"2a79506d34cacc7641946abda1785b7acf9910ad6a0978c91ec84d40b71d2891" \
	"379af19ffb333e7517e390bd26ac312fe940c340466b4a5d4af1d65c3b5944078" \
	"f96a1a51a5a53e4bc302818b7c9f63c4a1b07bd7d874cef1c3d4b2f5eb7871"
#define EXPONENT "10001"

/* 响应缓冲区 */
struct resp_buf {
	char *data;
	size_t len;
};

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct resp_buf *buf = userdata;
	size_t n = size * nmemb;
	char *p = realloc(buf->data, buf->len + n + 1);
	if (!p)
		return 0;
	buf->data = p;
	memcpy(buf->data + buf->len, ptr, n);
	buf->len += n;
	buf->data[buf->len] = '\0';
	return n;
}

/* HTTP GET，返回响应体（调用者 free），失败返回 NULL */
static char *http_get(const char *url)
{
	CURL *curl = curl_easy_init();
	struct resp_buf buf = {0};
	char *result = NULL;

	if (!curl)
		return NULL;

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "hust-network-login");

	if (curl_easy_perform(curl) == CURLE_OK) {
		result = buf.data ? buf.data : malloc(1);
		if (result)
			result[0] = '\0';
	} else {
		free(buf.data);
	}

	curl_easy_cleanup(curl);
	return result;
}

/* HTTP POST，返回响应体（调用者 free），失败返回 NULL */
static char *http_post(const char *url, const char *body)
{
	CURL *curl = curl_easy_init();
	struct resp_buf buf = {0};
	char *result = NULL;
	struct curl_slist *headers = NULL;

	if (!curl)
		return NULL;

	headers = curl_slist_append(headers,
		"Content-Type: application/x-www-form-urlencoded; charset=UTF-8");
	headers = curl_slist_append(headers, "Accept: */*");

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "hust-network-login");

	if (curl_easy_perform(curl) == CURLE_OK && buf.data)
		result = buf.data;
	else
		free(buf.data);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return result;
}

/*
 * RSA 加密：c = BE(password ">" mac) ^ e mod n，输出小写 hex，左补零到 256 字符。
 * 等价于 Rust 的 BigUint::from_bytes_be(...).modpow(65537, n)。
 */
static char *encrypt_pass(const char *password, const char *mac)
{
	char msg[256];
	BIGNUM *e = NULL, *n = NULL, *m = NULL, *c = NULL;
	BN_CTX *ctx = NULL;
	char *hex = NULL, *out = NULL;
	size_t len, zeros, i;

	snprintf(msg, sizeof(msg), "%s>%s", password, mac);

	ctx = BN_CTX_new();
	c = BN_new();
	BN_hex2bn(&e, EXPONENT);
	BN_hex2bn(&n, MODULUS);
	m = BN_bin2bn((const unsigned char *)msg, (int)strlen(msg), NULL);

	BN_mod_exp(c, m, e, n, ctx);

	hex = BN_bn2hex(c); /* 大写，无前导零 */
	len = strlen(hex);
	zeros = (len < 256) ? (256 - len) : 0;

	out = malloc(257);
	memset(out, '0', zeros);
	for (i = 0; i < len; i++)
		out[zeros + i] = (char)tolower((unsigned char)hex[i]);
	out[256] = '\0';

	OPENSSL_free(hex);
	BN_free(e);
	BN_free(n);
	BN_free(m);
	BN_free(c);
	BN_CTX_free(ctx);
	return out;
}

/* URL 编码（等价 Rust urlencoding::encode，大写 hex） */
static char *url_encode(const char *s)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t len = strlen(s), i, o = 0;
	char *out = malloc(len * 3 + 1);

	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out[o++] = (char)c;
		} else {
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 0xF];
		}
	}
	out[o] = '\0';
	return out;
}

/* 提取 prefix 与 suffix 之间的子串 */
static int extract(const char *text, const char *prefix, const char *suffix,
		   char *out, size_t outlen)
{
	const char *l = strstr(text, prefix);
	const char *r;
	size_t n;

	if (!l)
		return -1;
	l += strlen(prefix);
	r = strstr(l, suffix);
	if (!r)
		return -1;
	n = (size_t)(r - l);
	if (n >= outlen)
		return -1;
	memcpy(out, l, n);
	out[n] = '\0';
	return 0;
}

/* 去掉字符串末尾的 \r \n */
static void trim_crlf(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
		s[--n] = '\0';
}


/* 依次探测多个候选地址（逗号分隔），返回第一个能连上的响应体（调用者 free），全失败返回 NULL */
static char *probe(void)
{
	const char *p = test_url;

	while (*p) {
		char url[256];
		size_t i = 0;
		char *u, *resp;

		while (*p && *p != ',' && i < sizeof(url) - 1)
			url[i++] = *p++;
		url[i] = '\0';

		u = url;
		while (*u == ' ')
			u++;

		if (*u) {
			resp = http_get(u);
			if (resp)
				return resp;
			syslog(LOG_WARNING, "probe %s failed, trying next", u);
		}

		if (*p == ',')
			p++;
	}

	return NULL;
}

/* 一次登录：返回 0 表示已在线或登录成功，非 0 表示失败 */
static int login(const char *username, const char *password)
{
	char *resp, *enc, *qs_enc, *login_resp;
	char portal_ip[64], mac[128], query_string[1024];
	char body[2048], login_url[160];
	int ok = -1;

	write_state("probe");

	resp = probe();
	if (!resp) {
		set_error("all probe urls failed");
		return -1;
	}

	/* 响应不含门户标志 => 已在线 */
	if (!strstr(resp, "/eportal/index.jsp") &&
	    !strstr(resp, "<script>top.self.location.href='http://")) {
		free(resp);
		return 0;
	}

	if (extract(resp, "<script>top.self.location.href='http://",
		    "/eportal/index.jsp", portal_ip, sizeof(portal_ip)) != 0) {
		set_error("extract portal_ip failed");
		free(resp);
		return -1;
	}

	if (extract(resp, "mac=", "&t=", mac, sizeof(mac)) != 0) {
		set_error("extract mac failed");
		free(resp);
		return -1;
	}

	if (extract(resp, "/eportal/index.jsp?", "'</script>\r\n",
		    query_string, sizeof(query_string)) != 0) {
		set_error("extract query_string failed");
		free(resp);
		return -1;
	}

	enc = encrypt_pass(password, mac);
	qs_enc = url_encode(query_string);

	snprintf(body, sizeof(body),
		 "userId=%s&password=%s&service=&queryString=%s&passwordEncrypt=true",
		 username, enc, qs_enc);

	snprintf(login_url, sizeof(login_url),
		 "http://%s/eportal/InterFace.do?method=login", portal_ip);

	write_state("login");

	login_resp = http_post(login_url, body);
	if (login_resp) {
		syslog(LOG_INFO, "login resp: %.200s", login_resp);
		ok = strstr(login_resp, "success") ? 0 : -1;
		if (ok != 0)
			set_error("login rejected: %.160s", login_resp);
		free(login_resp);
	}
	else {
		set_error("login request failed");
	}

	free(enc);
	free(qs_enc);
	free(resp);
	return ok;
}

static int read_conf(const char *path, char *username, char *password)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(username, 128, f) || !fgets(password, 128, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	trim_crlf(username);
	trim_crlf(password);
	return 0;
}

int main(int argc, char **argv)
{
	char username[128] = {0}, password[128] = {0};

	openlog("hust-network-login", LOG_PID | LOG_NDELAY, LOG_DAEMON);

	/* SIGTERM/SIGINT = 干净退出；SIGHUP = 中断当前请求并立刻重新认证（重连不重启进程） */
	{
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = sig_handler;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0; /* 不设 SA_RESTART，让 sleep() 能被打断 */

		sigaction(SIGHUP, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
		sigaction(SIGINT, &sa, NULL);
	}

	{
		const char *tu = getenv("HUST_NETWORK_LOGIN_TEST_URL");
		const char *ci = getenv("HUST_NETWORK_LOGIN_CHECK_INTERVAL");
		if (tu && tu[0])
			test_url = tu;
		if (ci && ci[0]) {
			int v = atoi(ci);
			if (v > 0)
				check_interval = v;
		}
	}

	if (argc >= 2) {
		if (read_conf(argv[1], username, password) != 0) {
			set_error("failed to read config file: %s", argv[1]);
			write_state("error");
			return 1;
		}
	} else {
		const char *u = getenv("HUST_NETWORK_LOGIN_USERNAME");
		const char *p = getenv("HUST_NETWORK_LOGIN_PASSWORD");
		if (u)
			strncpy(username, u, sizeof(username) - 1);
		if (p)
			strncpy(password, p, sizeof(password) - 1);
	}

	if (!username[0] || !password[0]) {
		set_error("no username/password configured");
		write_state("error");
		fprintf(stderr,
			"no username/password. usage: %s [config_file]\n"
			"  config file: line1=username, line2=password\n"
			"  or env: HUST_NETWORK_LOGIN_USERNAME / HUST_NETWORK_LOGIN_PASSWORD\n",
			argv[0]);
		return 1;
	}

	curl_global_init(CURL_GLOBAL_ALL);

	write_pidfile();
	write_state("idle");

	while (!g_stop) {
		int rc;

		g_reconnect = 0; /* 消费掉上一次重连请求 */
		rc = login(username, password);

		if (g_stop)
			break;

		if (rc == 0) {
			syslog(LOG_INFO, "login ok, awaiting");
			last_error[0] = '\0';
			write_state("online");
			sleep_interruptible(check_interval);
		} else if (g_reconnect) {
			/* 手动重连打断的，不是错误：立刻进入下一轮 */
			syslog(LOG_INFO, "reconnect requested, restarting cycle");
		} else {
			syslog(LOG_ERR, "login failed, retry in 1s");
			write_state("error");
			sleep_interruptible(1);
		}
	}

	write_state("stopped");
	unlink(pid_file);
	curl_global_cleanup();
	syslog(LOG_INFO, "stopped");
	closelog();
	return 0;
}
