#ifndef ENDPOINT_H
#define ENDPOINT_H

#include <stdint.h>

#include "x_builtins.h"

enum endpoint_proto {
	PROTO_TCP,
	PROTO_UDP,
	PROTO_UNIX_STREAM,
	PROTO_UNIX_DGRAM,
	PROTO_FILE,
	PROTO_BUILTIN,
};

enum endpoint_kind {
	ENDPOINT_INET,
	ENDPOINT_UNIX,
	ENDPOINT_UNIX_DGRAM,
	ENDPOINT_BUILTIN,
	ENDPOINT_FILE,
};

struct endpoint {
	enum endpoint_kind kind;

	char host[256];
	uint16_t port;

	enum endpoint_proto proto;

	char path[108]; /* sockaddr_un sun_path limit on Linux */

	enum x_builtin_upstream builtin;
};

int endpoint_to_string(const struct endpoint *ep, char *buf, size_t buf_len);

bool endpoint_is_stream(const struct endpoint *ep);
bool endpoint_is_datagram(const struct endpoint *ep);
bool endpoint_is_listenable(const struct endpoint *ep);

#endif
