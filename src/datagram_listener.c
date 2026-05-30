#include <event2/event.h>

#include <string.h>

#include "klog.h"
#include "route.h"
#include "datagram_listener.h"
#include "datagram_client.h"
#include "datagram_builtin.h"

static int handle_datagram_builtin_packet(
	struct datagram_route_ctx *ctx,
	const struct datagram_packet *pkt)
{
	struct datagram_client tmp;
	memset(&tmp, 0, sizeof(tmp));

	tmp.ctx = ctx;
	tmp.fd = EVUTIL_INVALID_SOCKET;
	tmp.client_addr = pkt->peer_addr;
	tmp.client_addr_len = pkt->peer_addr_len;
	tmp.last_seen = time(NULL);

	int rc = start_datagram_builtin(&tmp);
	if (rc < 0) {
		LOG_ERROR("udp builtin failed", "err", _LOGV(strerror(-rc)));
	}
	return rc;
}

static struct datagram_client* datagram_route_get_or_create_client(
	struct datagram_route_ctx *ctx,
	const struct datagram_packet *pkt)
{
	struct datagram_client *c = find_datagram_client(ctx, &pkt->peer_addr, pkt->peer_addr_len);
	if (c == NULL) {
		c = create_datagram_client(ctx, &pkt->peer_addr, pkt->peer_addr_len);
		if (c == NULL) {
			return c;
		}
	}
	c->last_seen = time(NULL);
	return c;
}

static int datagram_route_handle_packet(
	struct datagram_route_ctx *ctx,
	const struct datagram_packet *pkt)
{
	struct datagram_client *c;
	int rc;

	if (pkt->route->upstream.kind == ENDPOINT_BUILTIN) {
		return handle_datagram_builtin_packet(ctx, pkt);
	}

	c = datagram_route_get_or_create_client(ctx, pkt);
	if (!c) {
		return -ENOMEM;
	}

	rc = send_datagram_payload_to_upstream(c, pkt->data, pkt->data_len);
	if (rc != 0) {
		LOG_WARN("failed to send datagram payload upstream", "err", _LOGV(strerror(-rc)));
		return rc;
	}

	return 0;
}

static int dispatch_datagram_packet(
	struct datagram_route_ctx *ctx,
	const struct datagram_packet *pkt
) {
	/*
	 * Datagram routes are pinned to ctx->base for now.
	 * Later this function can choose a worker/shard.
	 */
	return datagram_route_handle_packet(ctx, pkt);
}

static void listen_read_cb(evutil_socket_t fd, short events, void *arg)
{
	struct datagram_route_ctx *ctx = arg;
	struct datagram_packet pkt;
	ssize_t n;

	(void)events;

	cleanup_idle_datagram_clients(ctx);

	memset(&pkt, 0, sizeof(pkt));
	pkt.route = ctx->route;
	pkt.listen_fd = fd;
	pkt.peer_addr_len = sizeof(pkt.peer_addr);

	n = recvfrom(
		fd,
		(char *)pkt.data,
		sizeof(pkt.data),
		0,
		(struct sockaddr *)&pkt.peer_addr,
		&pkt.peer_addr_len
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

	pkt.data_len = (size_t)n;

	int rc = dispatch_datagram_packet(ctx, &pkt);
	if (rc != 0) {
		LOG_WARN("failed to dispatch datagram packet",
				"err", _LOGV(rc)
				);
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

	if (r->opts.broadcast_reply) {
		LOG_ERROR("broadcast_reply is only valid for udp inet listeners",
				"line", _LOGV(r->line_no));
		return -EINVAL;
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

	if (r->opts.broadcast_reply) {
		int yes = 1;
		if (setsockopt(ctx->listen_fd, SOL_SOCKET, SO_BROADCAST,
					(const char *)&yes, sizeof(yes)) < 0) {
			int err = EVUTIL_SOCKET_ERROR();
			LOG_ERROR("failed to enable broadcast",
					"line", _LOGV(r->line_no),
					"err", _LOGV(evutil_socket_error_to_string(err)));
			return -err;
		}
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
