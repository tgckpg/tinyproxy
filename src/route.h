#ifndef ROUTE_H
#define ROUTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "endpoint.h"

#define ROUTE_HOST_MAX 256
#define ROUTE_DEFAULT_IDLE_TIMEOUT_SEC 60
#define ROUTE_DEFAULT_CONNECT_TIMEOUT_SEC 5

struct route_options {
	bool proxy_v2;
	bool keep_alive;

	int idle_timeout_sec;
	int connect_timeout_sec;
};

struct route {
	struct endpoint listen;
	struct endpoint upstream;

	struct route_options opts;

	unsigned int line_no;
};

void route_options_str(const struct route_options *opts, char *buf, size_t buflen);

#endif
