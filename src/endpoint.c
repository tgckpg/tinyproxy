#include <stdbool.h>
#include <stdio.h>
#include <errno.h>

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
