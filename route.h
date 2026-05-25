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

typedef struct conn_s {
    struct bufferevent *client;
    struct bufferevent *upstream;
} conn_t;

struct listener_ctx {
    struct event_base *base;
    const struct route *route;
    struct evconnlistener *listener;
};

#endif
