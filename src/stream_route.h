#ifndef STREAM_ROUTE_H
#define STREAM_ROUTE_H

struct worker;
struct route;
struct event_base;
struct evconnlistener;

struct stream_route_ctx {
	struct event_base *accept_base;
	struct worker *worker;
	const struct route *route;
	struct evconnlistener *listener;
};

int start_stream_route(struct worker *w, const struct route *r,
	struct stream_route_ctx *ctx);

void stop_stream_route(struct stream_route_ctx *ctx);

#endif
