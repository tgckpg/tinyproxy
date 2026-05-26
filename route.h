#ifndef ROUTE_H
#define ROUTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct event_base;

#define ROUTE_HOST_MAX 256
#define ROUTE_DEFAULT_IDLE_TIMEOUT_SEC 60
#define ROUTE_DEFAULT_CONNECT_TIMEOUT_SEC 5

enum proto {
	PROTO_TCP,
	PROTO_UDP,
};

struct route_options {
	bool proxy_v2;
	bool keep_alive;

	int idle_timeout_sec;
	int connect_timeout_sec;
};

struct route {
	char listen_host[ROUTE_HOST_MAX];
	uint16_t listen_port;

	char upstream_host[ROUTE_HOST_MAX];
	uint16_t upstream_port;

	enum proto proto;
	struct route_options opts;

	unsigned int line_no;
};

struct worker {
	struct event_base *base;
	size_t id;
};

void route_options_str(const struct route_options *opts, char *buf, size_t buflen);

#endif
