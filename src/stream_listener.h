#ifndef STREAM_LISTENER_H
#define STREAM_LISTENER_H

#include "stream_route.h"

int bind_stream_listener(struct stream_route_ctx *ctx);
void accept_error_cb(struct evconnlistener *listener, void *arg);

#endif
