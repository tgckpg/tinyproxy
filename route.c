#include <stdio.h>
#include "route.h"

void route_options_str(const struct route *r, char *buf, size_t buflen)
{
	size_t n = 0;

	buf[0] = '\0';

#define ADD_OPT(name) do { \
		n += snprintf(buf + n, buflen - n, "%s%s", n == 0 ? "" : ", ", name); \
		if (n >= buflen) { \
			buf[buflen - 1] = '\0'; \
			return; \
		} \
	} while (0)

	if (r->send_proxy_v2) {
		ADD_OPT("proxy_v2");
	}

#undef ADD_OPT
}
