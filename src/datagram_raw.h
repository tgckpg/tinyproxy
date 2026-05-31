#ifndef DATAGRAM_RAW_H
#define DATAGRAM_RAW_H

#include <stddef.h>

#include <event2/util.h>

#include "route.h"

int datagram_raw_open_ipv4(evutil_socket_t *out_fd);

int datagram_raw_send_udp_ipv4(
	evutil_socket_t raw_fd,
	const struct endpoint *src_ep,
	const struct sockaddr *dst_addr,
	socklen_t dst_addr_len,
	const unsigned char *data,
	size_t data_len);

#endif
