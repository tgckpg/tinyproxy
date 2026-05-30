#ifndef DATAGRAM_LISTENER_H
#define DATAGRAM_LISTENER_H

#include "datagram_route.h"

int bind_datagram_listener(struct datagram_route_ctx *ctx);
void close_datagram_listener(struct datagram_route_ctx *ctx);

int datagram_route_handle_packet(
	struct worker *w,
	const struct worker_datagram_packet_msg *pkt);

#endif
