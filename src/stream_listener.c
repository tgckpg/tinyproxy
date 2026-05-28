#include <event2/listener.h>

#include <string.h>
#include "compat.h"

#include "klog.h"
#include "route.h"
#include "stream_listener.h"
#include "stream_conn.h"

static void accept_cb(
	struct evconnlistener *listener,
	evutil_socket_t client_fd,
	struct sockaddr *addr,
	int socklen,
	void *arg
) {
	(void)listener;

	struct stream_route_ctx *ctx = arg;
	struct accepted_client ac = {
		.fd = client_fd,
		.route = ctx->route,
	};

	if (addr != NULL && socklen > 0 && (size_t)socklen <= sizeof(ac.peer_addr)) {
		memcpy(&ac.peer_addr, addr, (size_t)socklen);
		ac.peer_addr_len = (socklen_t)socklen;
	} else {
		LOG_ERROR("invalid accepted client address", "socklen", _LOGV(socklen));
		evutil_closesocket(client_fd);
		return;
	}

	dispatch_client_fd(ctx->worker, &ac);
}

void accept_error_cb(struct evconnlistener *listener, void *arg)
{
	struct stream_route_ctx *ctx = arg;
	int err = EVUTIL_SOCKET_ERROR();

	LOG_ERROR("accept error", "err", _LOGV(evutil_socket_error_to_string(err)));

	evconnlistener_disable(listener);
	event_base_loopexit(ctx->accept_base, NULL);
}

static int bind_tcp_stream_listener(struct stream_route_ctx *ctx)
{
	const struct route *r = ctx->route;
	struct sockaddr_in listen_addr;

	memset(&listen_addr, 0, sizeof(listen_addr));
	listen_addr.sin_family = AF_INET;
	listen_addr.sin_port = htons(r->listen.port);

	if (inet_pton(AF_INET, r->listen.host, &listen_addr.sin_addr) != 1) {
		LOG_ERROR("invalid listen address",
			"listen", _LOGV_ENDPOINT(&r->listen));
		return -EINVAL;
	}

	ctx->listener = evconnlistener_new_bind(
		ctx->accept_base,
		accept_cb,
		ctx,
		LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
		-1,
		(struct sockaddr *)&listen_addr,
		sizeof(listen_addr)
	);

	if (ctx->listener == NULL) {
		LOG_ERROR("evconnlistener_new_bind failed",
			"listen", _LOGV_ENDPOINT(&r->listen));
		return -EADDRINUSE;
	}

	return 0;
}

#ifdef _WIN32
static int bind_unix_stream_listener(struct stream_route_ctx *ctx)
{
	(void)ctx;
	return -EAFNOSUPPORT;
}
#else
static int bind_unix_stream_listener(struct stream_route_ctx *ctx)
{
	const struct endpoint *ep = &ctx->route->listen;
	const char *path = ep->path;
	struct sockaddr_un sa;
	struct evconnlistener *listener;

	if (path[0] == '\0') {
		LOG_ERROR("empty unix stream listen path",
			"line", _LOGV(ctx->route->line_no),
			"listen", _LOGV_ENDPOINT(ep));
		return -EINVAL;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;

	if (strlen(path) >= sizeof(sa.sun_path)) {
		LOG_ERROR("unix stream listen path too long",
			"line", _LOGV(ctx->route->line_no),
			"path", _LOGV(path));
		return -ENAMETOOLONG;
	}

	memcpy(sa.sun_path, path, strlen(path) + 1);

	/*
	 * For listener sockets, stale socket files are common after crashes.
	 * This is safe enough for our config-driven proxy: if the path exists
	 * and is not ours, bind() would fail without this anyway.
	 */
	if (unlink(path) < 0 && errno != ENOENT) {
		LOG_ERROR("failed to remove existing unix stream socket",
			"line", _LOGV(ctx->route->line_no),
			"path", _LOGV(path),
			"err", _LOGV(strerror(errno)));
		return -errno;
	}

	listener = evconnlistener_new_bind(
		ctx->accept_base,
		accept_cb,
		ctx,
		LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
		-1,
		(struct sockaddr *)&sa,
		sizeof(sa)
	);
	if (listener == NULL) {
		int err = errno;

		LOG_ERROR("failed to bind unix stream listener",
			"line", _LOGV(ctx->route->line_no),
			"path", _LOGV(path),
			"err", _LOGV(strerror(err)));

		return err ? -err : -EIO;
	}

	ctx->listener = listener;
	return 0;
}

int bind_stream_listener(struct stream_route_ctx *ctx)
{
	switch (ctx->route->listen.kind) {
	case ENDPOINT_INET:
		return bind_tcp_stream_listener(ctx);

	case ENDPOINT_UNIX:
		return bind_unix_stream_listener(ctx);

	default:
		LOG_ERROR("stream listen protocol not implemented yet",
			"line", _LOGV(ctx->route->line_no),
			"listen", _LOGV_ENDPOINT(&ctx->route->listen));
		return  -ENOTSUP;
	}
}
#endif
