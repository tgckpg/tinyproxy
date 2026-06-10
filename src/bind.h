#ifndef BIND_H
#define BIND_H

#include <event2/util.h>

#include "route.h"

int bind_with_wait(
	evutil_socket_t fd,
	const struct sockaddr *addr,
	socklen_t addr_len,
	const struct route *r);

#endif
