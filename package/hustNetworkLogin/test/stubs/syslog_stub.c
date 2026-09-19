/* 把守护进程的 syslog() 抓成 stderr，方便离线测试里看到它到底在做什么
 * （沙箱里没有 syslogd，/dev/log 也不可写）。
 * 强符号定义在 .o 里，链接时优先于 libc 的同名符号。 */
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

void openlog(const char *ident, int option, int facility)
{
	(void)ident; (void)option; (void)facility;
}

void closelog(void)
{
}

void syslog(int priority, const char *format, ...)
{
	va_list ap;

	fprintf(stderr, "[syslog p%d] ", priority);
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	fflush(stderr);
}
