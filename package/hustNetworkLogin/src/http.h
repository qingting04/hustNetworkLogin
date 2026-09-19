/*
 * http.h —— 只够深澜 ePortal 用的明文 HTTP 客户端（GET / POST），零第三方依赖
 *
 * 为什么不用 libcurl：ImmortalWrt 的 curl 默认 SSL 后端是 OpenSSL（feeds/packages/net/curl/
 * Config.in 里 `default LIBCURL_OPENSSL`），于是「依赖 libcurl」等于「从源码编译整套
 * OpenSSL + curl」—— CI 的 Build 步 10.8 min 里绝大部分就是它们。而本程序对 HTTP 的
 * 全部需求只有：明文 GET 一个探测/门户页面、明文 POST 一个登录表单。这里用 socket
 * 直接实现，编译成本为 0，运行时也没多任何一个包。
 *
 * 与 curl 版的行为差异（都写在 README 的「可配置项」里）：
 *   - 只支持 http://（不支持 https://，遇到就报错并写日志）
 *   - 不跟随 3xx 跳转（curl 版也没设 FOLLOWLOCATION，行为一致）
 *   - 不解析 chunked 传输编码（门户与探测地址都用 Content-Length / 连接关闭收尾；
 *     万一遇到 chunked，正文里会多出分块长度行，但解析用的关键字匹配不受影响）
 *   - DNS 解析是阻塞的（与 curl 未启用 threaded-resolver 时相同）
 *
 * 中断语义与 curl 版一致：SIGHUP（手动重连）/SIGTERM 会让正在飞的请求立刻收摊 ——
 * 这里比 curl 的进度回调更直接：信号打断 poll()，每个等待切片都查一次中断标志。
 */
#ifndef HUST_NETWORK_LOGIN_HTTP_H
#define HUST_NETWORK_LOGIN_HTTP_H

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#define HTTP_CONNECT_TIMEOUT_MS 5000     /* 连接超时（curl 版是 CURLOPT_CONNECTTIMEOUT=5） */
#define HTTP_TOTAL_TIMEOUT_MS   10000    /* 整个请求的总超时（curl 版是 CURLOPT_TIMEOUT=10） */
#define HTTP_SLICE_MS           100      /* poll 切片：便于及时响应 SIGHUP/SIGTERM */
#define HTTP_MAX_BODY           (256 * 1024)
#define HTTP_USER_AGENT         "hust-network-login"

/* 由调用方注入的中断判据（返回非 0 = 立刻放弃本次请求） */
static int (*http_interrupt)(void);

/* ---------------------------------------------------------------- URL 解析 */

struct http_url {
	char host[256];
	char port[8];
	char path[512];
};

/* 解析 http://host[:port][/path]；失败（含 https、空 host）返回 -1 */
static int http_parse_url(const char *url, struct http_url *u)
{
	const char *p, *slash;
	char *colon;
	size_t n;

	memset(u, 0, sizeof(*u));

	if (!url || strncmp(url, "http://", 7) != 0)
		return -1;

	p = url + 7;
	slash = strchr(p, '/');
	n = slash ? (size_t)(slash - p) : strlen(p);

	if (n == 0 || n >= sizeof(u->host))
		return -1;

	memcpy(u->host, p, n);
	u->host[n] = '\0';

	colon = strrchr(u->host, ':');
	if (colon) {
		*colon = '\0';
		snprintf(u->port, sizeof(u->port), "%s", colon + 1);
	}

	if (!u->host[0] || !u->port[0])
		snprintf(u->port, sizeof(u->port), "80");

	snprintf(u->path, sizeof(u->path), "%s", slash ? slash : "/");

	return 0;
}

/* 把 HTTP 响应拆成头/体：返回正文起始下标，找不到头结束标记返回 -1 */
static ssize_t http_body_offset(const char *raw, size_t len)
{
	size_t i;

	for (i = 0; i + 3 < len; i++) {
		if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n')
			return (ssize_t)(i + 4);
	}

	for (i = 0; i + 1 < len; i++) {
		if (raw[i] == '\n' && raw[i + 1] == '\n')
			return (ssize_t)(i + 2);
	}

	return -1;
}

/* 从响应头里取 Content-Length（没有返回 -1） */
static long http_content_length(const char *raw, size_t head_len)
{
	size_t i;
	static const char key[] = "content-length:";

	for (i = 0; i + sizeof(key) - 1 < head_len; i++) {
		size_t j = 0;

		if (i > 0 && raw[i - 1] != '\n')
			continue;

		while (key[j] && i + j < head_len &&
		       ((unsigned char)raw[i + j] | 0x20) == (unsigned char)key[j])
			j++;

		if (key[j] == '\0')
			return strtol(raw + i + j, NULL, 10);
	}

	return -1;
}

/* ---------------------------------------------------------------- socket 小工具 */

/* 等 fd 就绪：1 = 就绪，0 = 超时，-1 = 出错或被中断 */
static int http_wait_fd(int fd, short events, int timeout_ms)
{
	int waited = 0;

	while (waited < timeout_ms) {
		struct pollfd pfd;
		int rc;

		pfd.fd = fd;
		pfd.events = events;
		pfd.revents = 0;

		rc = poll(&pfd, 1, HTTP_SLICE_MS);

		if (rc > 0)
			return 1;

		if (rc < 0) {
			if (errno == EINTR && !(http_interrupt && http_interrupt()))
				continue;

			return -1;
		}

		if (http_interrupt && http_interrupt())
			return -1;

		waited += HTTP_SLICE_MS;
	}

	return 0;
}

/* 非阻塞连接（带超时 + 可中断），失败返回 -1 */
static int http_connect(const struct http_url *u)
{
	struct addrinfo hints, *res = NULL, *ai;
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(u->host, u->port, &hints, &res) != 0)
		return -1;

	for (ai = res; ai; ai = ai->ai_next) {
		int flags, rc;

		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;

		flags = fcntl(fd, F_GETFL, 0);
		if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
			close(fd);
			fd = -1;
			continue;
		}

		rc = connect(fd, ai->ai_addr, ai->ai_addrlen);

		if (rc < 0 && errno == EINPROGRESS) {
			rc = http_wait_fd(fd, POLLOUT, HTTP_CONNECT_TIMEOUT_MS);

			if (rc == 1) {
				int err = 0;
				socklen_t elen = sizeof(err);

				if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0)
					rc = 0;
				else
					rc = -1;
			}
			else {
				rc = -1;
			}
		}
		else if (rc == 0) {
			rc = 0;
		}
		else {
			rc = -1;
		}

		if (rc == 0)
			break;

		close(fd);
		fd = -1;
	}

	freeaddrinfo(res);

	return fd;
}

/* 写完整个缓冲区，失败/中断返回 -1 */
static int http_send_all(int fd, const char *buf, size_t len)
{
	size_t off = 0;
	int waited = 0;

	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);

		if (n > 0) {
			off += (size_t)n;
			continue;
		}

		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (waited >= HTTP_TOTAL_TIMEOUT_MS)
				return -1;

			if (http_wait_fd(fd, POLLOUT, HTTP_SLICE_MS * 10) <= 0)
				return -1;

			waited += HTTP_SLICE_MS * 10;
			continue;
		}

		if (n < 0 && errno == EINTR && !(http_interrupt && http_interrupt()))
			continue;

		return -1;
	}

	return 0;
}

/* 收下整个响应（头 + 体），返回 malloc 的原始响应与长度；失败返回 NULL */
static char *http_recv_all(int fd, size_t *out_len)
{
	char *buf = NULL;
	size_t len = 0, cap = 0;
	long content_length = -1;
	size_t head_len = 0;
	int waited = 0;

	for (;;) {
		char chunk[4096];
		ssize_t n = read(fd, chunk, sizeof(chunk));

		if (n > 0) {
			if (len + (size_t)n > HTTP_MAX_BODY)
				break;

			if (len + (size_t)n + 1 > cap) {
				size_t ncap = cap ? cap * 2 : 8192;
				char *p;

				while (ncap < len + (size_t)n + 1)
					ncap *= 2;

				p = realloc(buf, ncap);
				if (!p)
					goto fail;

				buf = p;
				cap = ncap;
			}

			memcpy(buf + len, chunk, (size_t)n);
			len += (size_t)n;
			buf[len] = '\0';

			if (!head_len) {
				ssize_t off = http_body_offset(buf, len);

				if (off >= 0) {
					head_len = (size_t)off;
					content_length = http_content_length(buf, head_len);
				}
			}

			/* Content-Length 收齐就可以不等对端关闭了 */
			if (content_length >= 0 && len >= head_len + (size_t)content_length)
				break;

			continue;
		}

		if (n == 0)
			break;                       /* 对端关闭：Connection: close 下就是响应结束 */

		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			int rc = http_wait_fd(fd, POLLIN, HTTP_SLICE_MS);

			/* 这里按「空等时间」累计总超时：慢但对端一直有数据的传输不会被误杀
			 * （curl 版是 CURLOPT_TIMEOUT=10 的硬超时，行为上更宽松一点点） */
			waited += HTTP_SLICE_MS;

			if (rc < 0)
				break;                       /* 出错或被 SIGHUP/SIGTERM 打断 */

			if (waited >= HTTP_TOTAL_TIMEOUT_MS)
				break;                       /* 累计空等超过 10 秒 → 超时 */

			continue;
		}

		if (errno == EINTR && !(http_interrupt && http_interrupt()))
			continue;

		break;
	}

	/* 被 SIGHUP/SIGTERM 打断：这次请求作废，别把半截响应当成功 */
	if (http_interrupt && http_interrupt())
		goto fail;

	if (!buf || !len)
		goto fail;

	*out_len = len;

	return buf;

fail:
	free(buf);

	return NULL;
}

/* ---------------------------------------------------------------- 公开入口 */

/* 统一的一次请求：body 非空即 POST，否则 GET */
static char *http_request(const char *url, const char *body)
{
	struct http_url u;
	char *req = NULL, *raw = NULL;
	size_t reqlen, rawlen = 0;
	ssize_t off;
	char *out = NULL;
	size_t body_len = 0;
	int fd = -1;

	if (http_parse_url(url, &u) != 0) {
		if (url && strncmp(url, "https://", 8) == 0)
			syslog(LOG_WARNING, "http: %s 不支持 https（自带实现只做明文 HTTP）", url);
		else
			syslog(LOG_WARNING, "http: 无法解析 URL %s", url ? url : "(null)");

		return NULL;
	}

	if (body)
		body_len = strlen(body);

	reqlen = 512 + strlen(u.host) + strlen(u.path) + body_len;

	req = malloc(reqlen);
	if (!req)
		return NULL;

	if (body) {
		snprintf(req, reqlen,
			 "POST %s HTTP/1.1\r\n"
			 "Host: %s\r\n"
			 "User-Agent: " HTTP_USER_AGENT "\r\n"
			 "Accept: */*\r\n"
			 "Content-Type: application/x-www-form-urlencoded; charset=UTF-8\r\n"
			 "Content-Length: %zu\r\n"
			 "Connection: close\r\n"
			 "\r\n"
			 "%s",
			 u.path, u.host, body_len, body);
	}
	else {
		snprintf(req, reqlen,
			 "GET %s HTTP/1.1\r\n"
			 "Host: %s\r\n"
			 "User-Agent: " HTTP_USER_AGENT "\r\n"
			 "Accept: */*\r\n"
			 "Connection: close\r\n"
			 "\r\n",
			 u.path, u.host);
	}

	reqlen = strlen(req);

	fd = http_connect(&u);
	if (fd < 0)
		goto out;

	if (http_send_all(fd, req, reqlen) != 0)
		goto out;

	raw = http_recv_all(fd, &rawlen);
	if (!raw)
		goto out;

	off = http_body_offset(raw, rawlen);
	if (off < 0)
		goto out;

	out = malloc(rawlen - (size_t)off + 1);
	if (!out)
		goto out;

	memcpy(out, raw + off, rawlen - (size_t)off);
	out[rawlen - (size_t)off] = '\0';

out:
	free(raw);
	free(req);
	if (fd >= 0)
		close(fd);

	return out;
}

/* HTTP GET，返回响应体（调用者 free），失败返回 NULL */
static char *http_get(const char *url)
{
	return http_request(url, NULL);
}

/* HTTP POST，返回响应体（调用者 free），失败返回 NULL */
static char *http_post(const char *url, const char *body)
{
	return http_request(url, body);
}

#endif /* HUST_NETWORK_LOGIN_HTTP_H */
