#ifndef UDP_ROUTE_H
#define UDP_ROUTE_H

#include "route.h"

struct udp_route_ctx;

int start_udp_route(
	struct worker *w,
	const struct route *r,
	struct udp_route_ctx **out
);

void free_udp_route(struct udp_route_ctx *ctx);

#endif
