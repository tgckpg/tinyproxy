#include <event2/listener.h>

#include <string.h>
#include "compat.h"

#include "klog.h"
#include "route.h"
#include "worker_pool.h"
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

	struct worker *w = worker_pool_next(ctx->worker_pool);
	if (!w) {
		LOG_ERROR("no worker available", "socklen", _LOGV(socklen));
		evutil_closesocket(client_fd);
		return;
	}

	if (dispatch_client_fd(w, &ac) != 0) {
		evutil_closesocket(client_fd);
	}
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

static int bind_unix_stream_listener(struct stream_route_ctx *ctx)
{
	const struct endpoint *ep = &ctx->route->listen;
	const char *path = ep->path;
	struct sockaddr_un sa;
	evutil_socket_t fd = -1;
	struct evconnlistener *listener;
	size_t path_len;
	int rc;

	if (path[0] == '\0') {
		LOG_ERROR("empty unix stream listen path",
			"line", _LOGV(ctx->route->line_no),
			"listen", _LOGV_ENDPOINT(ep));
		return -EINVAL;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;

	path_len = strlen(path);
	if (path_len >= sizeof(sa.sun_path)) {
		LOG_ERROR("unix stream listen path too long",
			"line", _LOGV(ctx->route->line_no),
			"path", _LOGV(path));
		return -ENAMETOOLONG;
	}

	memcpy(sa.sun_path, path, path_len + 1);

	if (compat_unlink(path) < 0 && errno != ENOENT) {
		int err = errno;

		LOG_ERROR("failed to remove existing unix stream socket",
			"line", _LOGV(ctx->route->line_no),
			"path", _LOGV(path),
			"err", _LOGV(strerror(err)));
		return -err;
	}

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		int err = EVUTIL_SOCKET_ERROR();

		LOG_ERROR("failed to create unix stream listener socket",
			"line", _LOGV(ctx->route->line_no),
			"path", _LOGV(path),
			"err", _LOGV(evutil_socket_error_to_string(err)));
		return err ? -err : -EIO;
	}

#ifdef _WIN32
	/*
	 * Windows AF_UNIX is picky. Do not apply TCP-ish listener options
	 * such as SO_KEEPALIVE / SO_REUSEADDR here.
	 */
#else
	if (evutil_make_listen_socket_reuseable(fd) < 0) {
		int err = errno;

		LOG_ERROR("failed to make unix stream listener reusable",
			"line", _LOGV(ctx->route->line_no),
			"path", _LOGV(path),
			"err", _LOGV(strerror(err)));
		evutil_closesocket(fd);
		return -err;
	}
#endif

	if (evutil_make_socket_nonblocking(fd) < 0) {
		int err = EVUTIL_SOCKET_ERROR();

		LOG_ERROR("failed to make unix stream listener nonblocking",
			"line", _LOGV(ctx->route->line_no),
			"path", _LOGV(path),
			"err", _LOGV(evutil_socket_error_to_string(err)));
		evutil_closesocket(fd);
		return err ? -err : -EIO;
	}

	rc = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
	if (rc < 0) {
		int err = EVUTIL_SOCKET_ERROR();

		LOG_ERROR("failed to bind unix stream listener socket",
			"line", _LOGV(ctx->route->line_no),
			"path", _LOGV(path),
			"err", _LOGV(evutil_socket_error_to_string(err)));
		evutil_closesocket(fd);
		return err ? -err : -EIO;
	}

	listener = evconnlistener_new(
		ctx->accept_base,
		accept_cb,
		ctx,
		LEV_OPT_CLOSE_ON_FREE,
		-1,
		fd
	);
	if (listener == NULL) {
		int err = EVUTIL_SOCKET_ERROR();

		LOG_ERROR("failed to create unix stream listener",
			"line", _LOGV(ctx->route->line_no),
			"path", _LOGV(path),
			"err", _LOGV(evutil_socket_error_to_string(err)));
		evutil_closesocket(fd);
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
