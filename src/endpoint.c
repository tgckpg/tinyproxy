#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "endpoint.h"

int endpoint_to_string(const struct endpoint *ep, char *buf, size_t buf_len)
{
	if (buf == NULL || buf_len == 0) {
		return -EINVAL;
	}

	if (ep == NULL) {
		snprintf(buf, buf_len, "<nil>");
		return 0;
	}

	switch (ep->kind) {
	case ENDPOINT_INET:
		snprintf(buf, buf_len, "%s:%u", ep->host, ep->port);
		return 0;

	case ENDPOINT_INET6:
		snprintf(buf, buf_len, "[%s]:%u", ep->host, ep->port);
		return 0;

	case ENDPOINT_UNIX:
		snprintf(buf, buf_len, "unix:%s", ep->path);
		return 0;

	case ENDPOINT_UNIX_DGRAM:
		snprintf(buf, buf_len, "unix-dgram:%s", ep->path);
		return 0;

	case ENDPOINT_BUILTIN:
		snprintf(buf, buf_len, "builtin://%s", x_builtin_name(ep->builtin));
		return 0;

	case ENDPOINT_FILE:
		snprintf(buf, buf_len, "file://%s", ep->path);
		return 0;

	default:
		snprintf(buf, buf_len, "<unknown>");
		return 0;
	}
}

int endpoint_to_sockaddr(const struct endpoint *ep,
								struct sockaddr_storage *ss,
								socklen_t *ss_len)
{
	memset(ss, 0, sizeof(*ss));

	switch (ep->kind) {
	case ENDPOINT_INET: {
		struct sockaddr_in *addr4 = (struct sockaddr_in *)ss;

		addr4->sin_family = AF_INET;
		addr4->sin_port = htons(ep->port);

		if (inet_pton(AF_INET, ep->host, &addr4->sin_addr) != 1) {
			return -EINVAL;
		}

		*ss_len = sizeof(*addr4);
		return 0;
	}

	case ENDPOINT_INET6: {
		struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)ss;

		addr6->sin6_family = AF_INET6;
		addr6->sin6_port = htons(ep->port);

		if (inet_pton(AF_INET6, ep->host, &addr6->sin6_addr) != 1) {
			return -EINVAL;
		}

		*ss_len = sizeof(*addr6);
		return 0;
	}

	default:
		return -EINVAL;
	}
}

bool endpoint_is_stream(const struct endpoint *ep)
{
	if (ep == NULL) {
		return false;
	}

	switch (ep->proto) {
	case PROTO_TCP:
	case PROTO_UNIX_STREAM:
		return true;

	default:
		return false;
	}
}

bool endpoint_is_datagram(const struct endpoint *ep)
{
	if (ep == NULL) {
		return false;
	}

	switch (ep->proto) {
	case PROTO_UDP:
	case PROTO_UNIX_DGRAM:
		return true;

	default:
		return false;
	}
}

bool endpoint_is_listenable(const struct endpoint *ep)
{
	return endpoint_is_stream(ep) || endpoint_is_datagram(ep);
}
