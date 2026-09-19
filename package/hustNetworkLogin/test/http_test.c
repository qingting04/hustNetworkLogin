/*
 * http.h 的纯函数单测（不碰 socket，不开端口）
 *
 * 编译运行（CI 与本地都不需要任何第三方库）：
 *   cc -Wall -Wextra -Werror -Wno-unused-function \
 *      -o http_test package/hustNetworkLogin/test/http_test.c && ./http_test
 * 期望：全部 ok，退出码 0。
 *
 * 为什么关掉 unused-function：这个用例只测纯函数，http.h 里 connect/send/recv 那些
 * static 函数在本 TU 里没有任何调用点（它们在守护进程里才被用到），-Werror 下会误报。
 */
#include <stdio.h>
#include <string.h>

#include "../src/http.h"

static int fails;

static void ck(int ok, const char *what)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		fails++;
}

int main(void)
{
	struct http_url u;
	const char *r1 = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";
	const char *r2 = "HTTP/1.0 204 No Content\n\n";
	const char *r3 = "HTTP/1.1 200 OK\r\ncontent-length: 7\r\n\r\nbody123";
	const char *r4 = "HTTP/1.1 200 OK\r\nHost: x\r\n\r\n";   /* 有头没正文、没有 Content-Length */
	const char *r5 = "HTTP/1.0 200 OK\nContent-Length: 2\n\nhi";   /* LF-only 头 */

	/* ---- URL 解析：真实会用到的几种形态 ---- */
	ck(http_parse_url("http://10.0.0.1/portal?a=1", &u) == 0 &&
	   !strcmp(u.host, "10.0.0.1") && !strcmp(u.port, "80") && !strcmp(u.path, "/portal?a=1"),
	   "http://host/path（默认端口 80）");

	ck(http_parse_url("http://127.0.0.1:8080/generate_204", &u) == 0 &&
	   !strcmp(u.host, "127.0.0.1") && !strcmp(u.port, "8080") && !strcmp(u.path, "/generate_204"),
	   "http://host:port/path");

	ck(http_parse_url("http://portal.hust.edu.cn", &u) == 0 &&
	   !strcmp(u.host, "portal.hust.edu.cn") && !strcmp(u.port, "80") && !strcmp(u.path, "/"),
	   "没有 path 时补 \"/\"");

	ck(http_parse_url("https://example.com/x", &u) != 0, "拒绝 https://（自带实现只做明文）");
	ck(http_parse_url("ftp://example.com/x", &u) != 0, "拒绝非 http scheme");
	ck(http_parse_url("http://", &u) != 0, "拒绝空 host");
	ck(http_parse_url("", &u) != 0, "拒绝空串");
	ck(http_parse_url(NULL, &u) != 0, "拒绝 NULL");

	/* ---- 响应拆分：头/体边界按实际位置算，不写死偏移 ---- */
	ck(http_body_offset(r1, strlen(r1)) == (ssize_t)(strstr(r1, "\r\n\r\n") + 4 - r1) &&
	   !strcmp(r1 + (strstr(r1, "\r\n\r\n") + 4 - r1), "hi"),
	   "CRLF 头：正文起点正确（Body = \"hi\"）");

	ck(http_body_offset(r2, strlen(r2)) == (ssize_t)strlen(r2), "只有头、无正文 → 正文起点在末尾");
	ck(http_body_offset(r4, strlen(r4)) == (ssize_t)strlen(r4), "只发头不发正文 → 正文起点在末尾");
	ck(http_body_offset(r5, strlen(r5)) == (ssize_t)(strstr(r5, "\n\n") + 2 - r5) &&
	   !strcmp(r5 + (strstr(r5, "\n\n") + 2 - r5), "hi"), "LF-only 头同样能拆");
	ck(http_body_offset("garbage", 7) == -1, "没有头结束标记 → -1");

	/* ---- Content-Length 识别 ---- */
	ck(http_content_length(r1, strlen(r1)) == 2, "识别 Content-Length: 2");
	ck(http_content_length(r3, strlen(r3)) == 7, "大小写无关（content-length）");
	ck(http_content_length(r2, strlen(r2)) == -1, "204 没有 Content-Length → -1");
	ck(http_content_length(r4, strlen(r4)) == -1, "只有 Host 头没有 Content-Length → -1");
	ck(http_content_length(r5, strlen(r5)) == 2, "LF-only 头也能识别 Content-Length");

	printf("\n%s\n", fails ? "有失败" : "全部通过");
	return fails ? 1 : 0;
}
