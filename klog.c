#include "klog.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

void log_at(char sev, const char *file, int line, const char *fmt, ...)
{
	struct timespec ts;
	struct tm tm;
	char tbuf[32];

	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);

	strftime(tbuf, sizeof(tbuf), "%m%d %H:%M:%S", &tm);

	fprintf(stderr, "%c%s.%06ld %7ld %s:%d] ",
		sev,
		tbuf,
		ts.tv_nsec / 1000,
		(long)getpid(),
		file,
		line);

	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fputc('\n', stderr);
}
