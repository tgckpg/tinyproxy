#ifndef DATAGRAM_LISTENER_H
#define DATAGRAM_LISTENER_H

#include "datagram_route.h"

int bind_datagram_listener(struct datagram_route_ctx *ctx);
void close_datagram_listener(struct datagram_route_ctx *ctx);

#endif
