#ifndef STREAM_ROUTE_H
#define STREAM_ROUTE_H

#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/util.h>

#include "compat.h"

struct worker;
struct route;
struct event_base;
struct evconnlistener;

struct stream_route_ctx {
	struct event_base *accept_base;
	struct worker_pool *worker_pool;
	const struct route *route;
	struct evconnlistener *listener;
};

typedef struct conn_s {
	struct worker *owner;
	const struct route *route;

	struct bufferevent *client;
	struct bufferevent *upstream;

	struct sockaddr_storage peer_addr;
	socklen_t peer_addr_len;

	bool close_after_client_eof;
	bool close_client_after_drain;
} conn_t;

int start_stream_route(
		struct event_base *accept_base,
		struct worker_pool *wpool,
		const struct route *r,
		struct stream_route_ctx *ctx);

void stop_stream_route(struct stream_route_ctx *ctx);

#endif
