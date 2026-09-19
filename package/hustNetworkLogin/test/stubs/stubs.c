/* libcurl 与 syslog 桩实现（见 stubs/curl/curl.h）。
 * 目的：在开发机上把真正的 main.c 编出来、跑起来，验证 ubus 控制面。
 * 注意：main.c 的 RSA 公钥运算现在自带（src/modexp.h），不再需要 OpenSSL 桩。 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "curl/curl.h"

static int (*g_write_cb)(void *, size_t, size_t, void *);
static void *g_write_data;

CURL *curl_easy_init(void)
{
	return (CURL *)calloc(1, 8);
}

CURLcode curl_easy_setopt(CURL *h, int opt, ...)
{
	va_list ap;

	(void)h;
	va_start(ap, opt);

	switch (opt) {
	case CURLOPT_URL:
	case CURLOPT_POSTFIELDS:
	case CURLOPT_USERAGENT:
		(void)va_arg(ap, char *);
		break;
	case CURLOPT_WRITEFUNCTION:
		g_write_cb = (int (*)(void *, size_t, size_t, void *))va_arg(ap, void *);
		break;
	case CURLOPT_WRITEDATA:
		g_write_data = va_arg(ap, void *);
		break;
	case CURLOPT_HTTPHEADER:
		(void)va_arg(ap, struct curl_slist *);
		break;
	case CURLOPT_XFERINFOFUNCTION:
		(void)va_arg(ap, void *);
		break;
	case CURLOPT_POST:
	case CURLOPT_TIMEOUT:
	case CURLOPT_NOPROGRESS:
	case CURLOPT_CONNECTTIMEOUT:
		(void)va_arg(ap, long);
		break;
	default:
		break;
	}

	va_end(ap);
	return CURLE_OK;
}

CURLcode curl_easy_perform(CURL *h)
{
	/* 不含 "/eportal/index.jsp" 也不含门户跳转 script → 判定为已在线 */
	static const char body[] = "204 No Content";

	(void)h;
	if (g_write_cb)
		g_write_cb((void *)body, 1, sizeof(body) - 1, g_write_data);

	return CURLE_OK;
}

void curl_easy_cleanup(CURL *h)
{
	free(h);
}

void curl_global_init(long flags)
{
	(void)flags;
}

void curl_global_cleanup(void)
{
}

struct curl_slist *curl_slist_append(struct curl_slist *l, const char *s)
{
	(void)s;
	return l ? l : (struct curl_slist *)calloc(1, sizeof(struct curl_slist));
}

void curl_slist_free_all(struct curl_slist *l)
{
	free(l);
}
