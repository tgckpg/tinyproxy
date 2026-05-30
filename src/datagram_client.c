#include <event2/event.h>

#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "klog.h"
#include "env.h"
#include "route.h"
#include "datagram_client.h"
#include "proxy_proto_v2.h"

void free_datagram_client(struct datagram_client *c)
{
	if (c == NULL) {
		return;
	}

	if (c->ev != NULL) {
		event_free(c->ev);
	}

	if (c->fd >= 0) {
		evutil_closesocket(c->fd);
	}

	if (c->unix_local_path[0] != '\0') {
		unlink(c->unix_local_path);
	}

	free(c);
}

static int sockaddr_equal(
	const struct sockaddr_storage *a,
	socklen_t a_len,
	const struct sockaddr_storage *b,
	socklen_t b_len
) {
	if (a_len != b_len) {
		return 0;
	}

	if (a->ss_family != b->ss_family) {
		return 0;
	}

	return memcmp(a, b, (size_t)a_len) == 0;
}

void cleanup_idle_datagram_clients(struct datagram_route_ctx *ctx)
{
	time_t now = time(NULL);
	struct datagram_client **pp = &ctx->clients;

	while (*pp != NULL) {
		struct datagram_client *c = *pp;

		if (now - c->last_seen <= ctx->route->opts.idle_timeout_sec) {
			pp = &c->next;
			continue;
		}

		*pp = c->next;
		c->next = NULL;

		LOG_INFO("udp client expired",
			"client_family", _LOGV(c->client_addr.ss_family),
			"client_len", _LOGV(c->client_addr_len)
		);

		free_datagram_client(c);
	}
}

struct datagram_client *find_datagram_client(
	const struct datagram_route_ctx *ctx,
	const struct sockaddr_storage *addr,
	socklen_t addr_len
) {
	for (struct datagram_client *c = ctx->clients; c != NULL; c = c->next) {
		if (sockaddr_equal(&c->client_addr, c->client_addr_len, addr, addr_len)) {
			return c;
		}
	}

	return NULL;
}

static int connect_datagram_upstream(struct datagram_client *c, const struct endpoint *upstream)
{
	if (upstream == NULL) {
		return -EINVAL;
	}

	switch (upstream->kind) {

	case ENDPOINT_INET: {
		struct sockaddr_in upstream_addr;

		memset(&upstream_addr, 0, sizeof(upstream_addr));
		upstream_addr.sin_family = AF_INET;
		upstream_addr.sin_port = htons(upstream->port);

		if (inet_pton(AF_INET, upstream->host, &upstream_addr.sin_addr) != 1) {
			return -EINVAL;
		}

		c->fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (c->fd < 0) {
			return -EVUTIL_SOCKET_ERROR();
		}

		if (connect(
				c->fd,
				(const struct sockaddr *)&upstream_addr,
				sizeof(upstream_addr)
			) < 0) {
			return -EVUTIL_SOCKET_ERROR();
		}

		return 0;
	}

	case ENDPOINT_UNIX_DGRAM:
#ifdef _WIN32
		return -ENOTSUP;
#else
	{
		struct sockaddr_un local_addr;
		struct sockaddr_un upstream_addr;

		if (upstream->path[0] == '\0') {
			return -EINVAL;
		}

		if (strlen(upstream->path) >= sizeof(upstream_addr.sun_path)) {
			return -ENAMETOOLONG;
		}

		c->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
		if (c->fd < 0) {
			return -EVUTIL_SOCKET_ERROR();
		}

		memset(&local_addr, 0, sizeof(local_addr));
		local_addr.sun_family = AF_UNIX;

		// UNIX_DGRAM sock for reply
		const char *runtime_dir = tinyproxy_runtime_dir();

		int n = snprintf(
			c->unix_local_path,
			sizeof(c->unix_local_path),
			"%s/udp-%ld-%p.sock",
			runtime_dir,
			(long)getpid(),
			(void *)c
		);

		if (n < 0 || (size_t)n >= sizeof(c->unix_local_path)) {
			return -ENAMETOOLONG;
		}

		if (strlen(c->unix_local_path) >= sizeof(local_addr.sun_path)) {
			return -ENAMETOOLONG;
		}

		unlink(c->unix_local_path);
		strcpy(local_addr.sun_path, c->unix_local_path);

		if (bind(
				c->fd,
				(const struct sockaddr *)&local_addr,
				sizeof(local_addr)
			) < 0) {
			return -EVUTIL_SOCKET_ERROR();
		}

		memset(&upstream_addr, 0, sizeof(upstream_addr));
		upstream_addr.sun_family = AF_UNIX;
		strcpy(upstream_addr.sun_path, upstream->path);

		if (connect(
				c->fd,
				(const struct sockaddr *)&upstream_addr,
				sizeof(upstream_addr)
			) < 0) {
			return -EVUTIL_SOCKET_ERROR();
		}

		return 0;
	}
#endif

	default:
		return -ENOTSUP;
	}
}

int send_datagram_payload_to_upstream(
	struct datagram_client *c,
	const unsigned char *payload,
	size_t payload_len
) {
	const struct datagram_route_ctx *ctx = c->ctx;
	const struct route *r = ctx->route;

	if (!r->opts.proxy_v2) {
		ssize_t sent = send(c->fd, (const char *)payload, payload_len, 0);
		if (sent < 0) {
			return -EVUTIL_SOCKET_ERROR();
		}

		return 0;
	}

	if (c->client_addr.ss_family != AF_INET ||
		ctx->local_addr.ss_family != AF_INET) {
		LOG_ERROR("PROXY v2 UDP currently only supports IPv4",
			"client_family", _LOGV(c->client_addr.ss_family),
			"local_family", _LOGV(ctx->local_addr.ss_family)
		);
		return -EAFNOSUPPORT;
	}

	unsigned char hdr[256];
	size_t hdr_len = 0;

	int rc = proxy_v2_build(
		hdr,
		sizeof(hdr),
		(const struct sockaddr *)&c->client_addr,
		c->client_addr_len,
		(const struct sockaddr *)&ctx->local_addr,
		ctx->local_addr_len,
		SOCK_DGRAM,
		&hdr_len
	);
	if (rc != 0) {
		return rc;
	}

	if (hdr_len + payload_len > UDP_MAX_PACKET) {
		return -EMSGSIZE;
	}

	unsigned char out[UDP_MAX_PACKET];

	memcpy(out, hdr, hdr_len);
	memcpy(out + hdr_len, payload, payload_len);

	ssize_t sent = send(c->fd, (const char *)out, hdr_len + payload_len, 0);
	if (sent < 0) {
		return -EVUTIL_SOCKET_ERROR();
	}

	return 0;
}

static void upstream_read_cb(evutil_socket_t fd, short events, void *arg)
{
	(void)events;

	struct datagram_client *c = arg;
	const struct datagram_route_ctx *ctx = c->ctx;

	unsigned char buf[UDP_MAX_PACKET];

	for (;;) {
		ssize_t n = recv(fd, (char *)buf, sizeof(buf), 0);
		if (n < 0) {
			int err = EVUTIL_SOCKET_ERROR();

			if (socket_err_is_retriable(err)) {
				return;
			}

			LOG_ERROR("udp recvfrom failed",
				"err", _LOGV(evutil_socket_error_to_string(err))
			);
			return;
		}

		if (n == 0) {
			return;
		}

		ssize_t sent = sendto(
			ctx->listen_fd,
			(const char *)buf,
			(size_t)n,
			0,
			(const struct sockaddr *)&c->client_addr,
			c->client_addr_len
		);

		if (sent < 0) {
			int err = EVUTIL_SOCKET_ERROR();

			LOG_ERROR("udp send to client failed",
				"err", _LOGV(evutil_socket_error_to_string(err))
			);
			return;
		}
	}
}

struct datagram_client *create_datagram_client(
	struct datagram_route_ctx *ctx,
	struct event_base *base,
	const struct sockaddr_storage *client_addr,
	socklen_t client_addr_len)
{
	const struct route *r = ctx->route;

	struct datagram_client *c = calloc(1, sizeof(*c));
	if (c == NULL) {
		return NULL;
	}

	c->ctx = ctx;
	c->fd = -1;
	c->client_addr = *client_addr;
	c->client_addr_len = client_addr_len;
	c->last_seen = time(NULL);

	int rc = connect_datagram_upstream(c, &r->upstream);
	if (rc < 0) {
		LOG_ERROR("udp upstream connect failed",
			"upstream", _LOGV_ENDPOINT(&r->upstream),
			"err", _LOGV(evutil_socket_error_to_string(-rc))
		);
		free_datagram_client(c);
		return NULL;
	}

	if (evutil_make_socket_nonblocking(c->fd) < 0) {
		LOG_ERROR("evutil_make_socket_nonblocking failed");
		free_datagram_client(c);
		return NULL;
	}

	c->ev = event_new(base, c->fd, EV_READ | EV_PERSIST, upstream_read_cb, c);
	if (c->ev == NULL) {
		LOG_ERROR("event_new failed for udp upstream");
		free_datagram_client(c);
		return NULL;
	}

	if (event_add(c->ev, NULL) < 0) {
		LOG_ERROR("event_add failed for udp upstream");
		free_datagram_client(c);
		return NULL;
	}

	c->next = ctx->clients;
	ctx->clients = c;

	LOG_INFO("udp client created",
		"client_family", _LOGV(c->client_addr.ss_family),
		"client_len", _LOGV(c->client_addr_len)
	);

	return c;
}
