#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "proxy_proto_v2.h"

static uint16_t read_u16(const uint8_t *p)
{
	return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 1) {
		return 0;
	}

	struct sockaddr_storage src;
	struct sockaddr_storage dst;
	unsigned char buf[128];
	size_t out_len = 999999;

	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));
	memset(buf, 0xa5, sizeof(buf));

	int family = data[0] % 3;
	int sock_type = (size > 1 && (data[1] & 1)) ? SOCK_DGRAM : SOCK_STREAM;

	if (family == 0) {
		struct sockaddr_in *s = (struct sockaddr_in *)&src;
		struct sockaddr_in *d = (struct sockaddr_in *)&dst;

		s->sin_family = AF_INET;
		d->sin_family = AF_INET;

		if (size > 2) {
			memcpy(&s->sin_addr, data + 2, size - 2 > 4 ? 4 : size - 2);
		}
		if (size > 6) {
			memcpy(&d->sin_addr, data + 6, size - 6 > 4 ? 4 : size - 6);
		}
		if (size > 10) {
			memcpy(&s->sin_port, data + 10, size - 10 > 2 ? 2 : size - 10);
		}
		if (size > 12) {
			memcpy(&d->sin_port, data + 12, size - 12 > 2 ? 2 : size - 12);
		}

		size_t buf_len = size % sizeof(buf);

		int rc = proxy_v2_build(
			buf,
			buf_len,
			(const struct sockaddr *)s,
			sizeof(*s),
			(const struct sockaddr *)d,
			sizeof(*d),
			sock_type,
			&out_len
		);

		if (rc == 0) {
			if (out_len > buf_len) __builtin_trap();
			if (out_len != 28) __builtin_trap();
			if (memcmp(buf, PROXY_V2_SIG, 12) != 0) __builtin_trap();
			if (read_u16(buf + 14) != 12) __builtin_trap();
		}
	} else if (family == 1) {
		struct sockaddr_in6 *s = (struct sockaddr_in6 *)&src;
		struct sockaddr_in6 *d = (struct sockaddr_in6 *)&dst;

		s->sin6_family = AF_INET6;
		d->sin6_family = AF_INET6;

		if (size > 2) {
			memcpy(&s->sin6_addr, data + 2, size - 2 > 16 ? 16 : size - 2);
		}
		if (size > 18) {
			memcpy(&d->sin6_addr, data + 18, size - 18 > 16 ? 16 : size - 18);
		}
		if (size > 34) {
			memcpy(&s->sin6_port, data + 34, size - 34 > 2 ? 2 : size - 34);
		}
		if (size > 36) {
			memcpy(&d->sin6_port, data + 36, size - 36 > 2 ? 2 : size - 36);
		}

		size_t buf_len = size % sizeof(buf);

		int rc = proxy_v2_build(
			buf,
			buf_len,
			(const struct sockaddr *)s,
			sizeof(*s),
			(const struct sockaddr *)d,
			sizeof(*d),
			sock_type,
			&out_len
		);

		if (rc == 0) {
			if (out_len > buf_len) __builtin_trap();
			if (out_len != 52) __builtin_trap();
			if (memcmp(buf, PROXY_V2_SIG, 12) != 0) __builtin_trap();
			if (read_u16(buf + 14) != 36) __builtin_trap();
		}
	} else {
		struct sockaddr *s = (struct sockaddr *)&src;
		struct sockaddr *d = (struct sockaddr *)&dst;

		s->sa_family = AF_UNSPEC;
		d->sa_family = AF_UNSPEC;

		(void)proxy_v2_build(
			buf,
			size % sizeof(buf),
			s,
			sizeof(*s),
			d,
			sizeof(*d),
			sock_type,
			&out_len
		);
	}

	return 0;
}
