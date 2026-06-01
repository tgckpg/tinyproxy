#include <event2/event.h>

#include <string.h>

#include "klog.h"
#include "route.h"
#include "datagram_listener.h"
#include "datagram_client.h"
#include "datagram_builtin.h"
#include "datagram_raw.h"

#ifdef TINYPROXY_DEBUG
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#endif
static void tinyproxy_debug_race_sleep(const char *name)
{
	const char *want = getenv("TINYPROXY_RACE_SLEEP");
	const char *delay_s = getenv("TINYPROXY_RACE_SLEEP_US");
	long delay_us;

	if (!want || strcmp(want, name) != 0) {
		return;
	}

	delay_us = delay_s ? strtol(delay_s, NULL, 10) : 1000;
	if (delay_us <= 0) {
		delay_us = 1000;
	}

#ifdef _WIN32
	Sleep((DWORD)((delay_us + 999) / 1000));
#else
	usleep((useconds_t)delay_us);
#endif
}
#else
static void tinyproxy_debug_race_sleep(const char *name)
{
	(void)name;
}
#endif

static int handle_datagram_builtin_packet(const struct worker_datagram_packet_msg *pkt)
{
	struct datagram_client tmp;
	memset(&tmp, 0, sizeof(tmp));

	tmp.ctx = pkt->ctx;
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

static struct datagram_client *datagram_route_get_or_create_client(
	struct worker *w,
	const struct worker_datagram_packet_msg *pkt)
{
	struct datagram_client *c;

	c = find_datagram_client(pkt->ctx, &pkt->peer_addr, pkt->peer_addr_len);
	if (c == NULL) {
		tinyproxy_debug_race_sleep("udp_client_create");
		c = create_datagram_client(
			pkt->ctx,
			w->base,
			&pkt->peer_addr,
			pkt->peer_addr_len
		);
		if (c == NULL) {
			return NULL;
		}
	}

	c->last_seen = time(NULL);
	return c;
}

int datagram_route_handle_packet(
	struct worker *w,
	const struct worker_datagram_packet_msg *pkt)
{
	struct datagram_route_ctx *ctx = pkt->ctx;
	struct datagram_client *c;
	int rc;

	if (ctx->route->upstream.kind == ENDPOINT_BUILTIN) {
		return handle_datagram_builtin_packet(pkt);
	}

	compat_mutex_lock(&ctx->clients_mu);

	c = datagram_route_get_or_create_client(w, pkt);
	if (!c) {
		rc = -ENOMEM;
		goto out;
	}

	tinyproxy_debug_race_sleep("udp_before_send");

	rc = send_datagram_payload_to_upstream(c, pkt->data, pkt->data_len);

out:
	compat_mutex_unlock(&ctx->clients_mu);

	if (rc != 0) {
		LOG_WARN("failed to send datagram payload upstream",
				"listen", _LOGV_ENDPOINT(&ctx->route->listen),
				"upstream", _LOGV_ENDPOINT(&ctx->route->upstream),
				"err", _LOGV(strerror(-rc)));
	}

	return rc;
}

static int dispatch_datagram_packet(
	struct datagram_route_ctx *ctx,
	const struct sockaddr *peer_addr,
	socklen_t peer_addr_len,
	const unsigned char *data,
	size_t data_len)
{
	struct worker *w;

	if (!ctx || !ctx->worker_pool) {
		return EINVAL;
	}

	/*
	 * For now this can be round-robin to test plumbing.
	 * Before real UDP threading, change this to hash by peer_addr.
	 */
	w = worker_pool_next(ctx->worker_pool);
	if (!w) {
		return EINVAL;
	}

	return worker_enqueue_datagram_packet(
		w,
		ctx,
		peer_addr,
		peer_addr_len,
		data,
		data_len
	);
}

static void listen_read_cb(evutil_socket_t fd, short events, void *arg)
{
	struct datagram_route_ctx *ctx = arg;
	struct sockaddr_storage peer_addr;
	socklen_t peer_addr_len;
	unsigned char buf[65535];
	ssize_t n;
	int rc;

	(void)events;

	memset(&peer_addr, 0, sizeof(peer_addr));
	peer_addr_len = sizeof(peer_addr);

	n = recvfrom(
		fd,
		(char *)buf,
		sizeof(buf),
		0,
		(struct sockaddr *)&peer_addr,
		&peer_addr_len
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

	rc = dispatch_datagram_packet(
		ctx,
		(struct sockaddr *)&peer_addr,
		peer_addr_len,
		buf,
		(size_t)n
	);
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

	if (r->opts.broadcast_reply != BROADCAST_REPLY_OFF) {
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
	struct sockaddr_storage listen_addr;
	socklen_t listen_addr_len;
	int family;
	int one = 1;
	int rc;

	rc = endpoint_to_sockaddr(&r->listen, &listen_addr, &listen_addr_len);
	if (rc != 0) {
		LOG_ERROR("invalid udp listen address",
			"listen", _LOGV_ENDPOINT(&r->listen)
		);
		return rc;
	}

	switch (r->listen.kind) {
	case ENDPOINT_INET:
		family = AF_INET;
		break;

	case ENDPOINT_INET6:
		family = AF_INET6;
		break;

	default:
		LOG_ERROR("invalid udp listen endpoint kind",
			"listen", _LOGV_ENDPOINT(&r->listen)
		);
		return -EINVAL;
	}

	ctx->listen_fd = socket(family, SOCK_DGRAM, 0);
	if (ctx->listen_fd < 0) {
		int err = EVUTIL_SOCKET_ERROR();

		LOG_ERROR("udp listen socket failed",
			"err", _LOGV(evutil_socket_error_to_string(err))
		);
		return -err;
	}

	if (r->opts.broadcast_reply != BROADCAST_REPLY_OFF) {
		if (r->listen.kind != ENDPOINT_INET) {
			LOG_ERROR("broadcast_reply is only supported for IPv4 UDP",
				"line", _LOGV(r->line_no),
				"listen", _LOGV_ENDPOINT(&r->listen)
			);
			return -EINVAL;
		}

		{
			int yes = 1;

			if (setsockopt(ctx->listen_fd, SOL_SOCKET, SO_BROADCAST,
						(const char *)&yes, sizeof(yes)) < 0) {
				int err = EVUTIL_SOCKET_ERROR();

				LOG_ERROR("failed to enable broadcast",
					"line", _LOGV(r->line_no),
					"err", _LOGV(evutil_socket_error_to_string(err))
				);
				return -err;
			}
		}
	}

	if (family == AF_INET6) {
		int yes = 1;

		/*
		 * Keep behavior explicit:
		 *   udp :1234      => IPv4 only
		 *   udp [::]:1234  => IPv6 only
		 */
		if (setsockopt(ctx->listen_fd, IPPROTO_IPV6, IPV6_V6ONLY,
					(const char *)&yes, sizeof(yes)) < 0) {
			int err = EVUTIL_SOCKET_ERROR();

			LOG_ERROR("failed to set IPV6_V6ONLY",
				"line", _LOGV(r->line_no),
				"err", _LOGV(evutil_socket_error_to_string(err))
			);
			return -err;
		}
	}

	evutil_make_socket_closeonexec(ctx->listen_fd);

	if (evutil_make_socket_nonblocking(ctx->listen_fd) < 0) {
		LOG_ERROR("evutil_make_socket_nonblocking failed");
		return -EINVAL;
	}

	if (setsockopt(
			ctx->listen_fd,
			SOL_SOCKET,
			SO_REUSEADDR,
			(const char *)&one,
			sizeof(one)
		) < 0) {
		int err = EVUTIL_SOCKET_ERROR();

		LOG_ERROR("failed to set SO_REUSEADDR",
			"line", _LOGV(r->line_no),
			"err", _LOGV(evutil_socket_error_to_string(err))
		);
		return -err;
	}

	if (bind(
			ctx->listen_fd,
			(const struct sockaddr *)&listen_addr,
			listen_addr_len
		) < 0) {
		int err = EVUTIL_SOCKET_ERROR();

		LOG_ERROR("udp bind failed",
			"err", _LOGV(evutil_socket_error_to_string(err))
		);
		return -err;
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
		return -err;
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
	case ENDPOINT_INET6:
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
