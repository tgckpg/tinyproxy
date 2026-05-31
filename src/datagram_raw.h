#ifndef DATAGRAM_RAW_H
#define DATAGRAM_RAW_H

#include <stddef.h>

#include <event2/util.h>

#include "route.h"

struct raw_ipv4_hdr {
	uint8_t  vhl;
	uint8_t  tos;
	uint16_t len;
	uint16_t id;
	uint16_t off;
	uint8_t  ttl;
	uint8_t  proto;
	uint16_t sum;
	uint32_t src;
	uint32_t dst;
};

struct raw_udp_hdr {
	uint16_t sport;
	uint16_t dport;
	uint16_t len;
	uint16_t sum;
};

int datagram_raw_open_ipv4(evutil_socket_t *out_fd);

int datagram_raw_send_udp_ipv4(
	evutil_socket_t raw_fd,
	const struct endpoint *src_ep,
	const struct sockaddr *dst_addr,
	socklen_t dst_addr_len,
	const unsigned char *data,
	size_t data_len);

#endif
