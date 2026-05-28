#include <stdio.h>
#include <string.h>

#include "route.h"
#include "x_builtins.h"
#include "datagram_builtin.h"

static const char *datagram_client_addr_string(
	const struct datagram_client *c,
	char *buf,
	size_t buf_len
) {
	void *addr;
	uint16_t port;

	if (c == NULL || buf == NULL || buf_len == 0) {
		return NULL;
	}

	switch (c->client_addr.ss_family) {
	case AF_INET: {
		const struct sockaddr_in *sin =
			(const struct sockaddr_in *)&c->client_addr;

		addr = (void *)&sin->sin_addr;
		port = ntohs(sin->sin_port);

		if (inet_ntop(AF_INET, addr, buf, buf_len) == NULL) {
			return NULL;
		}

		break;
	}

	case AF_INET6: {
		const struct sockaddr_in6 *sin6 =
			(const struct sockaddr_in6 *)&c->client_addr;

		addr = (void *)&sin6->sin6_addr;
		port = ntohs(sin6->sin6_port);

		if (inet_ntop(AF_INET6, addr, buf, buf_len) == NULL) {
			return NULL;
		}

		break;
	}

	default:
		if (snprintf(buf, buf_len, "unknown") < 0) {
			return NULL;
		}
		return buf;
	}

	if (snprintf(buf + strlen(buf), buf_len - strlen(buf), ":%u", port) < 0) {
		return NULL;
	}

	return buf;
}

int start_datagram_builtin(struct datagram_client *c)
{
	struct x_builtin_request req;
	struct x_builtin_response res;
	char client_addr[128];
	int rc;

	if (c == NULL || c->ctx == NULL || c->ctx->route == NULL) {
		return -EINVAL;
	}

	memset(&req, 0, sizeof(req));

	req.builtin = c->ctx->route->upstream.builtin;
	req.client_addr = datagram_client_addr_string(c, client_addr, sizeof(client_addr));

	rc = x_builtin_handle(&req, &res);
	if (rc < 0) {
		return rc;
	}

	switch (res.action) {
	case X_BUILTIN_ACTION_CLOSE:
		if (res.data_len == 0) {
			return 0;
		}

		if (sendto(
			    c->ctx->listen_fd,
			    res.data,
			    res.data_len,
			    0,
			    (const struct sockaddr *)&c->client_addr,
			    c->client_addr_len) < 0) {
			return -EVUTIL_SOCKET_ERROR();
		}

		return 0;

	case X_BUILTIN_ACTION_DISCARD:
		return 0;

	case X_BUILTIN_ACTION_HANG:
		return 0;

	default:
		return -EINVAL;
	}
}
