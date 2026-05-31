#ifndef DATAGRAM_ROUTE_H
#define DATAGRAM_ROUTE_H

#include <event2/util.h>

#include "compat.h"
#include "compat_thread.h"
#include "worker_pool.h"

struct event;
struct event_base;
struct worker;
struct route;
struct datagram_client;

struct datagram_route_ctx {
	compat_mutex_t clients_mu;

	struct event_base *base;
	struct worker_pool *worker_pool;
	const struct route *route;

	evutil_socket_t listen_fd;
	struct event *listen_ev;

	struct sockaddr_storage local_addr;
	socklen_t local_addr_len;

	struct datagram_client *clients;

	evutil_socket_t raw_fd;
};

int start_datagram_route(
		struct event_base *accept_base,
		struct worker_pool *wpool,
		const struct route *r,
		struct datagram_route_ctx *ctx);

void stop_datagram_route_listener(struct datagram_route_ctx *ctx);
void free_datagram_route(struct datagram_route_ctx *ctx);

static inline int socket_is_valid(evutil_socket_t fd)
{
    return fd != (evutil_socket_t)EVUTIL_INVALID_SOCKET;
}

#endif
