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
};

struct worker {
	struct event_base *base;
	size_t id;
};

struct accepted_client {
	evutil_socket_t fd;
	struct sockaddr_storage peer_addr;
	socklen_t peer_addr_len;
	const struct route *route;
};

typedef struct conn_s {
	struct worker *owner;
	const struct route *route;

	struct bufferevent *client;
	struct bufferevent *upstream;

	struct sockaddr_storage peer_addr;
	socklen_t peer_addr_len;
} conn_t;

struct listener_ctx {
	struct event_base *accept_base;
	struct worker *worker;
	const struct route *route;
	struct evconnlistener *listener;
};

#endif
