#include "datagram_raw.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "endpoint.h"

static uint16_t checksum16(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t sum = 0;

	while (len >= 2) {
		sum += ((uint16_t)p[0] << 8) | p[1];
		p += 2;
		len -= 2;
	}

	if (len != 0) {
		sum += ((uint16_t)p[0] << 8);
	}

	while ((sum >> 16) != 0) {
		sum = (sum & 0xffffu) + (sum >> 16);
	}

	return (uint16_t)~sum;
}

#ifndef _WIN32

struct udp4_pseudo_header {
	uint32_t src;
	uint32_t dst;
	uint8_t zero;
	uint8_t proto;
	uint16_t len;
};

static uint16_t udp4_checksum(
	const struct ip *ip,
	const struct udphdr *udp,
	const unsigned char *payload,
	size_t payload_len)
{
	struct udp4_pseudo_header ph;
	size_t udp_len = sizeof(*udp) + payload_len;
	size_t total_len = sizeof(ph) + udp_len;
	unsigned char *buf;
	uint16_t sum;

	if (udp_len > UINT16_MAX || total_len > SIZE_MAX) {
		return 0;
	}

	buf = malloc(total_len);
	if (!buf) {
		return 0;
	}

	memset(&ph, 0, sizeof(ph));
	ph.src = ip->ip_src.s_addr;
	ph.dst = ip->ip_dst.s_addr;
	ph.zero = 0;
	ph.proto = IPPROTO_UDP;
	ph.len = htons((uint16_t)udp_len);

	memcpy(buf, &ph, sizeof(ph));
	memcpy(buf + sizeof(ph), udp, sizeof(*udp));
	memcpy(buf + sizeof(ph) + sizeof(*udp), payload, payload_len);

	sum = checksum16(buf, total_len);
	free(buf);

	/*
	 * For IPv4 UDP, checksum 0 means "no checksum".
	 * If the computed checksum is 0, transmit it as 0xffff.
	 */
	if (sum == 0) {
		sum = 0xffff;
	}

	return sum;
}

int datagram_raw_open_ipv4(evutil_socket_t *out_fd)
{
	evutil_socket_t fd;
	int one = 1;

	if (!out_fd) {
		return -EINVAL;
	}

	fd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
	if (fd < 0) {
		return -errno;
	}

	if (setsockopt(fd, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one)) < 0) {
		int err = errno;
		evutil_closesocket(fd);
		return -err;
	}

	*out_fd = fd;
	return 0;
}

static int endpoint_to_sockaddr_in(
	const struct endpoint *ep,
	struct sockaddr_in *out)
{
	if (!ep || !out) {
		return -EINVAL;
	}

	if (ep->kind != ENDPOINT_INET) {
		return -EINVAL;
	}

	memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	out->sin_port = htons(ep->port);

	if (inet_pton(AF_INET, ep->host, &out->sin_addr) != 1) {
		return -EINVAL;
	}

	return 0;
}

int datagram_raw_replay_ipv4(
	evutil_socket_t raw_fd,
	const struct route *r,
	const struct sockaddr *peer_addr,
	socklen_t peer_addr_len,
	const unsigned char *data,
	size_t data_len)
{
	const struct sockaddr_in *src;
	struct sockaddr_in upstream_addr;
	struct sockaddr_in send_dst;
	unsigned char *packet;
	struct ip *ip;
	struct udphdr *udp;
	size_t ip_len = sizeof(*ip);
	size_t udp_len = sizeof(*udp);
	size_t packet_len;
	ssize_t n;
	int rc;

	(void)peer_addr_len;

	if (raw_fd < 0 || !r || !peer_addr || !data) {
		return -EINVAL;
	}

	if (peer_addr->sa_family != AF_INET) {
		return -EAFNOSUPPORT;
	}

	rc = endpoint_to_sockaddr_in(&r->upstream, &upstream_addr);
	if (rc != 0) {
		return rc;
	}

	src = (const struct sockaddr_in *)peer_addr;

	if (data_len > UINT16_MAX - ip_len - udp_len) {
		return -EMSGSIZE;
	}

	packet_len = ip_len + udp_len + data_len;

	packet = calloc(1, packet_len);
	if (!packet) {
		return -ENOMEM;
	}

	ip = (struct ip *)packet;
	udp = (struct udphdr *)(packet + ip_len);

	ip->ip_v = 4;
	ip->ip_hl = 5;
	ip->ip_tos = 0;
	ip->ip_len = htons((uint16_t)packet_len);
	ip->ip_id = 0;
	ip->ip_off = 0;
	ip->ip_ttl = 64;
	ip->ip_p = IPPROTO_UDP;
	ip->ip_src = src->sin_addr;
	ip->ip_dst = upstream_addr.sin_addr;
	ip->ip_sum = 0;
	ip->ip_sum = checksum16(ip, ip_len);

	udp->uh_sport = src->sin_port;
	udp->uh_dport = upstream_addr.sin_port;
	udp->uh_ulen = htons((uint16_t)(udp_len + data_len));
	udp->uh_sum = 0;

	memcpy(packet + ip_len + udp_len, data, data_len);

	udp->uh_sum = udp4_checksum(ip, udp, data, data_len);

	memset(&send_dst, 0, sizeof(send_dst));
	send_dst.sin_family = AF_INET;
	send_dst.sin_addr = upstream_addr.sin_addr;
	send_dst.sin_port = upstream_addr.sin_port;

	n = sendto(
		raw_fd,
		(const char *)packet,
		packet_len,
		0,
		(const struct sockaddr *)&send_dst,
		sizeof(send_dst));

	free(packet);

	if (n < 0) {
		return -errno;
	}

	if ((size_t)n != packet_len) {
		return -EIO;
	}

	return 0;
}

#else

int datagram_raw_open_ipv4(evutil_socket_t *out_fd)
{
	(void)out_fd;
	return -ENOTSUP;
}

int datagram_raw_replay_ipv4(
	evutil_socket_t raw_fd,
	const struct route *r,
	const struct sockaddr *peer_addr,
	socklen_t peer_addr_len,
	const unsigned char *data,
	size_t data_len)
{
	(void)raw_fd;
	(void)r;
	(void)peer_addr;
	(void)peer_addr_len;
	(void)data;
	(void)data_len;

	return -ENOTSUP;
}

#endif
