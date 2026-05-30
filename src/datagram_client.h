#ifndef DATAGRAM_CLIENT_H
#define DATAGRAM_CLIENT_H

#include "datagram_route.h"

struct datagram_client {
	struct datagram_route_ctx *ctx;

	evutil_socket_t fd;
	struct event *ev;

	char unix_local_path[108];

	struct sockaddr_storage client_addr;
	socklen_t client_addr_len;

	time_t last_seen;

	struct datagram_client *next;
};

void cleanup_idle_datagram_clients(struct datagram_route_ctx *ctx);
void free_datagram_client(struct datagram_client *c);

struct datagram_client *create_datagram_client(
	struct datagram_route_ctx *ctx,
	struct event_base *base,
	const struct sockaddr_storage *client_addr,
	socklen_t client_addr_len);

struct datagram_client *find_datagram_client(
	const struct datagram_route_ctx *ctx,
	const struct sockaddr_storage *addr,
	socklen_t addr_len
);

int send_datagram_payload_to_upstream(
	struct datagram_client *c,
	const unsigned char *payload,
	size_t payload_len
);

#endif
