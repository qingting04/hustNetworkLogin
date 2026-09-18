/*
 * hust-network-login (C 版)
 * 华中科技大学校园网（深澜 ePortal）自动登录，掉线 15 秒自动重连。
 *
 * 依赖：libcurl（HTTP）、libcrypto/libopenssl（RSA 大数运算）。
 * 配置：环境变量 HUST_NETWORK_LOGIN_USERNAME / HUST_NETWORK_LOGIN_PASSWORD，
 *       或命令行传配置文件路径（两行：第一行用户名，第二行密码）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <ctype.h>
#include <curl/curl.h>
#include <openssl/bn.h>

/* 在线探测地址：默认用轻量的 captive portal 检测地址（在线时返回 204 空响应），
 * 掉线时同样会被门户拦截并返回带 query string 的页面。
 * 可用环境变量 HUST_NETWORK_LOGIN_TEST_URL 覆盖。 */
static const char *test_url = "http://connect.rom.miui.com/generate_204";
/* 在线检测间隔（秒），可用环境变量 HUST_NETWORK_LOGIN_CHECK_INTERVAL 覆盖 */
static int check_interval = 15;

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
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
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
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
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

/* 一次登录：返回 0 表示已在线或登录成功，非 0 表示失败 */
static int login(const char *username, const char *password)
{
	char *resp, *enc, *qs_enc, *login_resp;
	char portal_ip[64], mac[128], query_string[1024];
	char body[2048], login_url[160];
	int ok = -1;

	resp = http_get(test_url);
	if (!resp) {
		syslog(LOG_ERR, "get %s failed", test_url);
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
		syslog(LOG_ERR, "extract portal_ip failed");
		free(resp);
		return -1;
	}

	if (extract(resp, "mac=", "&t=", mac, sizeof(mac)) != 0) {
		syslog(LOG_ERR, "extract mac failed");
		free(resp);
		return -1;
	}

	if (extract(resp, "/eportal/index.jsp?", "'</script>\r\n",
		    query_string, sizeof(query_string)) != 0) {
		syslog(LOG_ERR, "extract query_string failed");
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

	login_resp = http_post(login_url, body);
	if (login_resp) {
		syslog(LOG_INFO, "login resp: %.200s", login_resp);
		ok = strstr(login_resp, "success") ? 0 : -1;
		free(login_resp);
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
			syslog(LOG_ERR, "failed to read config file: %s", argv[1]);
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
		syslog(LOG_ERR, "no username/password configured");
		fprintf(stderr,
			"no username/password. usage: %s [config_file]\n"
			"  config file: line1=username, line2=password\n"
			"  or env: HUST_NETWORK_LOGIN_USERNAME / HUST_NETWORK_LOGIN_PASSWORD\n",
			argv[0]);
		return 1;
	}

	curl_global_init(CURL_GLOBAL_ALL);

	for (;;) {
		if (login(username, password) == 0) {
			syslog(LOG_INFO, "login ok, awaiting");
			sleep(check_interval);
		} else {
			syslog(LOG_ERR, "login failed, retry in 1s");
			sleep(1);
		}
	}

	/* unreachable */
	curl_global_cleanup();
	return 0;
}
