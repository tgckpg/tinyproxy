#ifndef TCP_ROUTE_H
#define TCP_ROUTE_H

#include "route.h"

int start_tcp_route(
    struct event_base *base,
    const struct route *r,
    struct listener_ctx **out);

void free_tcp_route(struct listener_ctx *ctx);
#endif
