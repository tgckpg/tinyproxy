#ifndef DATAGRAM_ROUTE_H
#define DATAGRAM_ROUTE_H

#include <event2/util.h>

#include "compat.h"
#include "worker_pool.h"

struct event;
struct event_base;
struct worker;
struct route;
struct datagram_client;

struct datagram_route_ctx {
	struct event_base *base;
	struct worker_pool *worker_pool;
	const struct route *route;

	evutil_socket_t listen_fd;
	struct event *listen_ev;

	struct sockaddr_storage local_addr;
	socklen_t local_addr_len;

	struct datagram_client *clients;
};

int start_datagram_route(
		struct event_base *accept_base,
		struct worker_pool *wpool,
		const struct route *r,
		struct datagram_route_ctx *ctx);

void stop_datagram_route(struct datagram_route_ctx *ctx);

#endif
