#ifndef ROUTE_H
#define ROUTE_H

#include <event2/bufferevent.h>
#include <stdbool.h>

enum proto {
	PROTO_TCP,
	PROTO_UDP,
};

struct route {
	char listen_host[64];
	uint16_t listen_port;

	char upstream_host[64];
	uint16_t upstream_port;

	enum proto proto;
	bool send_proxy_v2;

	unsigned int line_no;
};

struct worker {
	struct event_base *base;
	size_t id;
};

void route_options_str(const struct route *r, char *buf, size_t buflen);

#endif
