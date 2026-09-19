/* 离线测试用的 libcurl 桩：只满足 main.c 用到的接口，perform 直接回一段
 * 没有门户标志的响应体（于是守护进程判定「已在线」），不产生真实网络流量。 */
#ifndef _STUB_CURL_H
#define _STUB_CURL_H

#include <stddef.h>

typedef struct _CURL CURL;
typedef int CURLcode;
typedef long curl_off_t;

struct curl_slist {
	char *data;
	struct curl_slist *next;
};

enum {
	CURLOPT_URL = 10002,
	CURLOPT_WRITEDATA = 10001,
	CURLOPT_WRITEFUNCTION = 20011,
	CURLOPT_USERAGENT = 10018,
	CURLOPT_HTTPHEADER = 10023,
	CURLOPT_POSTFIELDS = 10015,
	CURLOPT_TIMEOUT = 13,
	CURLOPT_NOPROGRESS = 43,
	CURLOPT_POST = 47,
	CURLOPT_CONNECTTIMEOUT = 78,
	CURLOPT_XFERINFOFUNCTION = 20219,
};

#define CURLE_OK 0
#define CURL_GLOBAL_ALL 0

CURL *curl_easy_init(void);
CURLcode curl_easy_setopt(CURL *h, int opt, ...);
CURLcode curl_easy_perform(CURL *h);
void curl_easy_cleanup(CURL *h);
void curl_global_init(long flags);
void curl_global_cleanup(void);
struct curl_slist *curl_slist_append(struct curl_slist *l, const char *s);
void curl_slist_free_all(struct curl_slist *l);

#endif
