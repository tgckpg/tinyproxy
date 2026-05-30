#ifndef ROUTE_RUNTIME_H
#define ROUTE_RUNTIME_H

#include "route.h"
#include "stream_route.h"
#include "datagram_route.h"

enum route_ctx_kind {
	ROUTE_CTX_NONE,
	ROUTE_CTX_STREAM,
	ROUTE_CTX_DATAGRAM,
};

struct route_ctx {
	enum route_ctx_kind kind;
	const struct route *route;

	union {
		struct stream_route_ctx stream;
		struct datagram_route_ctx datagram;
	} u;
};

int validate_route(const struct route *r);

int start_route(
		struct event_base *accept_base,
		struct worker_pool *wp,
		const struct route *r,
		struct route_ctx *ctx);

void stop_route(struct route_ctx *ctx);

#endif
