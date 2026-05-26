#include <stdio.h>
#include "route.h"

void route_options_str(const struct route_options *opts, char *buf, size_t buflen)
{
	size_t n = 0;

	if (buflen == 0) {
		return;
	}

	buf[0] = '\0';

#define ADD_FMT(fmt, ...) do { \
		int written; \
		if (n >= buflen) { \
			buf[buflen - 1] = '\0'; \
			return; \
		} \
		written = snprintf( \
			buf + n, \
			buflen - n, \
			"%s" fmt, \
			n == 0 ? "" : ", ", \
			__VA_ARGS__ \
		); \
		if (written < 0) { \
			buf[buflen - 1] = '\0'; \
			return; \
		} \
		if ((size_t)written >= buflen - n) { \
			n = buflen - 1; \
			buf[n] = '\0'; \
			return; \
		} \
		n += (size_t)written; \
	} while (0)

#define ADD_BOOL(name, enabled) \
	ADD_FMT("%s=%s", name, (enabled) ? "true" : "false")

#define ADD_INT(name, value) \
	ADD_FMT("%s=%d", name, value)

	ADD_BOOL("proxy_v2", opts->proxy_v2);
	ADD_INT("idle_timeout", opts->idle_timeout_sec);
	ADD_INT("connect_timeout", opts->connect_timeout_sec);
	ADD_BOOL("keep_alive", opts->keep_alive);

#undef ADD_INT
#undef ADD_BOOL
#undef ADD_FMT
}
