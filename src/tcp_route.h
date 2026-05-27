#ifndef TCP_ROUTE_H
#define TCP_ROUTE_H

#include "route.h"
#ifdef FUZZ
#include "compat_socket.h"
#endif

struct tcp_route_ctx;

int start_tcp_route(
	struct worker *w,
	const struct route *r,
	struct tcp_route_ctx **out);

void free_tcp_route(struct tcp_route_ctx *ctx);
#endif

#ifdef FUZZ
int tcp_route_adopt_client_for_fuzz(
	struct event_base *base,
	const struct route *r,
	evutil_socket_t client_fd,
	const struct sockaddr_storage *peer_addr,
	socklen_t peer_addr_len
);
#endif
