#include <event2/bufferevent.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "proxy_proto_v2.h"

static void put_u16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)(v & 0xff);
}

int proxy_v2_build(
	unsigned char *buf,
	size_t buf_len,
	const struct sockaddr *src,
	socklen_t src_len,
	const struct sockaddr *dst,
	socklen_t dst_len,
	int sock_type,
	size_t *out_len
) {
	unsigned char fam_proto;
	uint16_t addr_len;
	unsigned char *p;

	if (buf == NULL || src == NULL || dst == NULL || out_len == NULL) {
		return -EINVAL;
	}

	if (src->sa_family != dst->sa_family) {
		return -EAFNOSUPPORT;
	}

	if (sock_type == SOCK_STREAM) {
		fam_proto = PP2_TRANS_STREAM;
	} else if (sock_type == SOCK_DGRAM) {
		fam_proto = PP2_TRANS_DGRAM;
	} else {
		return -EPROTONOSUPPORT;
	}

	switch (src->sa_family) {
	case AF_INET:
		if (src_len < sizeof(struct sockaddr_in) ||
		    dst_len < sizeof(struct sockaddr_in)) {
			return -EINVAL;
		}

		addr_len = 12;

		if (buf_len < 16 + addr_len) {
			return -ENOSPC;
		}

		memcpy(buf, PROXY_V2_SIG, 12);
		buf[12] = PP2_VERSION_CMD_PROXY;
		buf[13] = PP2_FAM_INET | fam_proto;
		put_u16(buf + 14, addr_len);

		p = buf + 16;

		const struct sockaddr_in *s4 = (const struct sockaddr_in *)src;
		const struct sockaddr_in *d4 = (const struct sockaddr_in *)dst;

		memcpy(p, &s4->sin_addr, 4);
		p += 4;

		memcpy(p, &d4->sin_addr, 4);
		p += 4;

		memcpy(p, &s4->sin_port, 2);
		p += 2;

		memcpy(p, &d4->sin_port, 2);
		p += 2;

		*out_len = 16 + addr_len;
		return 0;

	case AF_INET6:
		if (src_len < sizeof(struct sockaddr_in6) ||
		    dst_len < sizeof(struct sockaddr_in6)) {
			return -EINVAL;
		}

		addr_len = 36;

		if (buf_len < 16 + addr_len) {
			return -ENOSPC;
		}

		memcpy(buf, PROXY_V2_SIG, 12);
		buf[12] = PP2_VERSION_CMD_PROXY;
		buf[13] = PP2_FAM_INET6 | fam_proto;
		put_u16(buf + 14, addr_len);

		p = buf + 16;

		const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)src;
		const struct sockaddr_in6 *d6 = (const struct sockaddr_in6 *)dst;

		memcpy(p, &s6->sin6_addr, 16);
		p += 16;

		memcpy(p, &d6->sin6_addr, 16);
		p += 16;

		memcpy(p, &s6->sin6_port, 2);
		p += 2;

		memcpy(p, &d6->sin6_port, 2);
		p += 2;

		*out_len = 16 + addr_len;
		return 0;

	default:
		return -EAFNOSUPPORT;
	}
}

int proxy_v2_write_bufferevent(
	struct bufferevent *bev,
	const struct sockaddr *src,
	socklen_t src_len,
	const struct sockaddr *dst,
	socklen_t dst_len,
	int sock_type
) {
	unsigned char hdr[256];
	size_t hdr_len = 0;

	int rc = proxy_v2_build(
		hdr,
		sizeof(hdr),
		src,
		src_len,
		dst,
		dst_len,
		sock_type,
		&hdr_len
	);
	if (rc != 0) {
		return rc;
	}

	if (bufferevent_write(bev, hdr, hdr_len) < 0) {
		return -EIO;
	}

	return 0;
}
