#include <event2/event.h>

#include <string.h>

#include "klog.h"
#include "route.h"
#include "datagram_listener.h"
#include "datagram_client.h"
#include "datagram_builtin.h"
#include "proxy_proto_v2.h"

static int send_datagram_payload_to_upstream(
	struct datagram_client *c,
	const unsigned char *payload,
	size_t payload_len
) {
	struct datagram_route_ctx *ctx = c->ctx;
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

static void listen_read_cb(evutil_socket_t fd, short events, void *arg)
{
	(void)events;

	struct datagram_route_ctx *ctx = arg;

	cleanup_idle_datagram_clients(ctx);

	for (;;) {
		unsigned char buf[UDP_MAX_PACKET];

		struct sockaddr_storage client_addr;
		socklen_t client_addr_len = sizeof(client_addr);

		memset(&client_addr, 0, sizeof(client_addr));

		ssize_t n = recvfrom(
			fd,
			(char *)buf,
			sizeof(buf),
			0,
			(struct sockaddr *)&client_addr,
			&client_addr_len
		);

		if (n < 0) {
			int err = EVUTIL_SOCKET_ERROR();

			if (socket_err_is_retriable(err)) {
				return;
			}

			LOG_ERROR("udp listen recv failed",
				"err", _LOGV(evutil_socket_error_to_string(err))
			);
			return;
		}

		if (n == 0) {
			continue;
		}

		if (ctx->route->upstream.kind == ENDPOINT_BUILTIN) {
			struct datagram_client tmp;

			memset(&tmp, 0, sizeof(tmp));
			tmp.ctx = ctx;
			tmp.fd = -1;
			tmp.client_addr = client_addr;
			tmp.client_addr_len = client_addr_len;
			tmp.last_seen = time(NULL);

			int rc = start_datagram_builtin(&tmp);
			if (rc < 0) {
				LOG_ERROR("udp builtin failed",
						"err", _LOGV(evutil_socket_error_to_string(-rc))
						);
				return;
			}

			continue;
		}

		struct datagram_client *c = find_datagram_client(ctx, &client_addr, client_addr_len);
		if (c == NULL) {
			c = create_datagram_client(ctx, &client_addr, client_addr_len);
			if (c == NULL) {
				return;
			}
		}

		c->last_seen = time(NULL);

		int rc = send_datagram_payload_to_upstream(c, buf, (size_t)n);
		if (rc < 0) {
			LOG_ERROR("udp send to upstream failed",
				"err", _LOGV(evutil_socket_error_to_string(-rc))
			);
			return;
		}
	}
}

#ifndef _WIN32
static int bind_unix_datagram_listener(struct datagram_route_ctx *ctx)
{
	const struct route *r = ctx->route;
	const char *path = r->listen.path;
	struct sockaddr_un listen_addr;

	if (path[0] == '\0') {
		LOG_ERROR("empty unix datagram listen path",
			"line", _LOGV(r->line_no),
			"listen", _LOGV_ENDPOINT(&r->listen));
		return -EINVAL;
	}

	if (strlen(path) >= sizeof(listen_addr.sun_path)) {
		LOG_ERROR("unix datagram listen path too long",
			"line", _LOGV(r->line_no),
			"path", _LOGV(path));
		return -ENAMETOOLONG;
	}

	memset(&listen_addr, 0, sizeof(listen_addr));
	listen_addr.sun_family = AF_UNIX;
	memcpy(listen_addr.sun_path, path, strlen(path) + 1);

	if (unlink(path) < 0 && errno != ENOENT) {
		LOG_ERROR("failed to remove existing unix datagram listener socket",
			"line", _LOGV(r->line_no),
			"path", _LOGV(path),
			"err", _LOGV(strerror(errno)));
		return -errno;
	}

	ctx->listen_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (ctx->listen_fd < 0) {
		int err = errno;

		LOG_ERROR("unix datagram listen socket failed",
			"line", _LOGV(r->line_no),
			"path", _LOGV(path),
			"err", _LOGV(strerror(err)));
		return -err;
	}

	evutil_make_socket_closeonexec(ctx->listen_fd);

	if (evutil_make_socket_nonblocking(ctx->listen_fd) < 0) {
		int err = errno;

		LOG_ERROR("evutil_make_socket_nonblocking failed for unix datagram listener",
			"line", _LOGV(r->line_no),
			"path", _LOGV(path),
			"err", _LOGV(strerror(err)));
		return err ? -err : -EINVAL;
	}

	if (bind(
			ctx->listen_fd,
			(const struct sockaddr *)&listen_addr,
			sizeof(listen_addr)
		) < 0) {
		int err = errno;

		LOG_ERROR("unix datagram bind failed",
			"line", _LOGV(r->line_no),
			"path", _LOGV(path),
			"err", _LOGV(strerror(err)));
		return -err;
	}

	ctx->local_addr_len = sizeof(ctx->local_addr);
	memset(&ctx->local_addr, 0, sizeof(ctx->local_addr));

	if (getsockname(
			ctx->listen_fd,
			(struct sockaddr *)&ctx->local_addr,
			&ctx->local_addr_len
		) < 0) {
		int err = errno;

		LOG_ERROR("unix datagram getsockname failed",
			"line", _LOGV(r->line_no),
			"path", _LOGV(path),
			"err", _LOGV(strerror(err)));
		return err ? -err : -EINVAL;
	}

	ctx->listen_ev = event_new(
		ctx->base,
		ctx->listen_fd,
		EV_READ | EV_PERSIST,
		listen_read_cb,
		ctx
	);
	if (ctx->listen_ev == NULL) {
		LOG_ERROR("event_new failed for unix datagram listener",
			"line", _LOGV(r->line_no),
			"path", _LOGV(path));
		return -ENOMEM;
	}

	if (event_add(ctx->listen_ev, NULL) < 0) {
		LOG_ERROR("event_add failed for unix datagram listener",
			"line", _LOGV(r->line_no),
			"path", _LOGV(path));
		return -EINVAL;
	}

	return 0;
}
#else
static int bind_unix_datagram_listener(struct datagram_route_ctx *ctx)
{
	(void)ctx;
	return -ENOTSUP;
}
#endif

static int bind_udp_datagram_listener(struct datagram_route_ctx *ctx)
{
	const struct route *r = ctx->route;
	struct sockaddr_in listen_addr;
	int one = 1;

	memset(&listen_addr, 0, sizeof(listen_addr));

	listen_addr.sin_family = AF_INET;
	listen_addr.sin_port = htons(r->listen.port);

	if (inet_pton(AF_INET, r->listen.host, &listen_addr.sin_addr) != 1) {
		LOG_ERROR("invalid udp listen address",
			"listen", _LOGV_ENDPOINT(&r->listen)
		);
		return -EINVAL;
	}

	ctx->listen_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (ctx->listen_fd < 0) {
		int err = EVUTIL_SOCKET_ERROR();

		LOG_ERROR("udp listen socket failed",
			"err", _LOGV(evutil_socket_error_to_string(err))
		);
		return -err;
	}

	evutil_make_socket_closeonexec(ctx->listen_fd);

	if (evutil_make_socket_nonblocking(ctx->listen_fd) < 0) {
		LOG_ERROR("evutil_make_socket_nonblocking failed");
		return -EINVAL;
	}

	setsockopt(
		ctx->listen_fd,
		SOL_SOCKET,
		SO_REUSEADDR,
		(const char *)&one,
		sizeof(one)
	);

	if (bind(
			ctx->listen_fd,
			(const struct sockaddr *)&listen_addr,
			sizeof(listen_addr)
		) < 0) {
		int err = EVUTIL_SOCKET_ERROR();

		LOG_ERROR("udp bind failed",
			"err", _LOGV(evutil_socket_error_to_string(err))
		);
		return -EADDRINUSE;
	}

	ctx->local_addr_len = sizeof(ctx->local_addr);
	memset(&ctx->local_addr, 0, sizeof(ctx->local_addr));

	if (getsockname(
			ctx->listen_fd,
			(struct sockaddr *)&ctx->local_addr,
			&ctx->local_addr_len
		) < 0) {
		int err = EVUTIL_SOCKET_ERROR();

		LOG_ERROR("udp getsockname failed",
			"err", _LOGV(evutil_socket_error_to_string(err))
		);
		return -EINVAL;
	}

	ctx->listen_ev = event_new(
		ctx->base,
		ctx->listen_fd,
		EV_READ | EV_PERSIST,
		listen_read_cb,
		ctx
	);
	if (ctx->listen_ev == NULL) {
		LOG_ERROR("event_new failed for udp listener");
		return -ENOMEM;
	}

	if (event_add(ctx->listen_ev, NULL) < 0) {
		LOG_ERROR("event_add failed for udp listener");
		return -EINVAL;
	}

	return 0;
}

int bind_datagram_listener(struct datagram_route_ctx *ctx)
{
	switch (ctx->route->listen.kind) {
	case ENDPOINT_INET:
		return bind_udp_datagram_listener(ctx);

	case ENDPOINT_UNIX_DGRAM:
		return bind_unix_datagram_listener(ctx);

	default:
		LOG_ERROR("datagram listen protocol not implemented yet",
			"line", _LOGV(ctx->route->line_no),
			"listen", _LOGV_ENDPOINT(&ctx->route->listen));
		return -ENOTSUP;
	}
}
