#ifndef TCP_ROUTE_H
#define TCP_ROUTE_H

#include "route.h"

struct tcp_route_ctx;

int start_tcp_route(
	struct worker *w,
	const struct route *r,
	struct tcp_route_ctx **out);

void free_tcp_route(struct tcp_route_ctx *ctx);
#endif
