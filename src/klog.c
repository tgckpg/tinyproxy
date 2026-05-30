#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "compat.h"

#include "klog.h"
#include "route.h"

static _Thread_local int klog_worker_id = -1;

void klog_set_worker_id(int id)
{
	klog_worker_id = id;
}

static int localtime_compat(const time_t *t, struct tm *out)
{
#ifdef _WIN32
	return localtime_s(out, t);
#else
	return localtime_r(t, out) == NULL ? -1 : 0;
#endif
}

static void print_quoted_value(const char *s)
{
	if (s == NULL) {
		fputs("\"\"", stderr);
		return;
	}

	fputc('"', stderr);

	for (; *s != '\0'; s++) {
		switch (*s) {
		case '\\':
			fputs("\\\\", stderr);
			break;
		case '"':
			fputs("\\\"", stderr);
			break;
		case '\n':
			fputs("\\n", stderr);
			break;
		case '\r':
			fputs("\\r", stderr);
			break;
		case '\t':
			fputs("\\t", stderr);
			break;
		default:
			fputc(*s, stderr);
			break;
		}
	}

	fputc('"', stderr);
}

static void log_print_endpoint(FILE *out, const struct endpoint *ep)
{
	char buf[256];

	endpoint_to_string(ep, buf, sizeof(buf));
	fputs(buf, out);
}

static void print_log_value(struct log_value value)
{
	switch (value.type) {
	case LOG_VALUE_STR:
		print_quoted_value(value.v.s);
		break;
	case LOG_VALUE_INT:
		fprintf(stderr, "%d", value.v.i);
		break;
	case LOG_VALUE_UINT:
		fprintf(stderr, "%u", value.v.u);
		break;
	case LOG_VALUE_LONG:
		fprintf(stderr, "%ld", value.v.l);
		break;
	case LOG_VALUE_ULONG:
		fprintf(stderr, "%lu", value.v.ul);
		break;
	case LOG_VALUE_LLONG:
		fprintf(stderr, "%lld", value.v.ll);
		break;
	case LOG_VALUE_ULLONG:
		fprintf(stderr, "%llu", value.v.ull);
		break;
	case LOG_VALUE_ENDPOINT:
		log_print_endpoint(stderr, value.v.endpoint);
		break;
	default:
		fputs("<invalid>", stderr);
		break;
	}
}

void log_at(char sev, const char *file, int line, const char *msg, ...)
{
#ifdef FUZZ
	return;
#endif

	struct timespec ts;
	struct tm tm;
	char tbuf[32];

	clock_gettime(CLOCK_REALTIME, &ts);
	if (localtime_compat(&ts.tv_sec, &tm) != 0) {
		memset(&tm, 0, sizeof(tm));
	}

	strftime(tbuf, sizeof(tbuf), "%m%d %H:%M:%S", &tm);

	fprintf(stderr, "%c%s.%06ld %7ld %s:%d] worker=%u msg=",
		sev,
		tbuf,
		ts.tv_nsec / 1000,
		(long)compat_getpid(),
		file,
		line,
		klog_worker_id);

	print_quoted_value(msg);

	va_list ap;
	va_start(ap, msg);

	for (;;) {
		const char *key = va_arg(ap, const char *);
		if (key == NULL) {
			break;
		}

		struct log_value value = va_arg(ap, struct log_value);

		fprintf(stderr, " %s=", key);
		print_log_value(value);
	}

	va_end(ap);

	fputc('\n', stderr);
}
