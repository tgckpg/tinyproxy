#include <event2/listener.h>

#include <string.h>
#include "compat.h"

#include "klog.h"
#include "route.h"
#include "bind.h"
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
	struct stream_route_ctx *ctx = arg;
	struct worker *w;
	int rc;

	(void)listener;

	if (addr == NULL || socklen <= 0 || (size_t)socklen > sizeof(struct sockaddr_storage)) {
		LOG_ERROR("invalid accepted client address",
			"socklen", _LOGV(socklen)
		);
		evutil_closesocket(client_fd);
		return;
	}

	w = worker_pool_next(ctx->worker_pool);
	if (!w) {
		LOG_ERROR("no worker available", "socklen", _LOGV(socklen));
		evutil_closesocket(client_fd);
		return;
	}

	rc = dispatch_client_fd(
		w,
		ctx->route,
		client_fd,
		addr,
		(socklen_t)socklen
	);
	if (rc != 0) {
		LOG_ERROR("failed to dispatch accepted client",
			"worker", _LOGV(w->id),
			"err", _LOGV(rc)
		);
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
	struct sockaddr_storage listen_addr;
	socklen_t listen_addr_len;
	evutil_socket_t fd;
	struct evconnlistener *listener;
	int rc;

	rc = endpoint_to_sockaddr(&r->listen, &listen_addr, &listen_addr_len);
	if (rc != 0) {
		LOG_ERROR("invalid listen address",
			"listen", _LOGV_ENDPOINT(&r->listen));
		return rc;
	}

	fd = socket(listen_addr.ss_family, SOCK_STREAM, 0);
	if (fd < 0) {
		int err = EVUTIL_SOCKET_ERROR();

		LOG_ERROR("failed to create tcp listener socket",
			"listen", _LOGV_ENDPOINT(&r->listen),
			"err", _LOGV(evutil_socket_error_to_string(err)));
		return err ? -err : -EIO;
	}

	if (evutil_make_listen_socket_reuseable(fd) < 0) {
		int err = EVUTIL_SOCKET_ERROR();

		LOG_ERROR("failed to make tcp listener reusable",
			"listen", _LOGV_ENDPOINT(&r->listen),
			"err", _LOGV(evutil_socket_error_to_string(err)));
		evutil_closesocket(fd);
		return err ? -err : -EIO;
	}

	if (evutil_make_socket_nonblocking(fd) < 0) {
		int err = EVUTIL_SOCKET_ERROR();

		LOG_ERROR("failed to make tcp listener nonblocking",
			"listen", _LOGV_ENDPOINT(&r->listen),
			"err", _LOGV(evutil_socket_error_to_string(err)));
		evutil_closesocket(fd);
		return err ? -err : -EIO;
	}

	rc = bind_with_wait(
		fd,
		(struct sockaddr *)&listen_addr,
		listen_addr_len,
		r
	);
	if (rc != 0) {
		int err = -rc;

		LOG_ERROR("failed to bind tcp listener socket",
			"listen", _LOGV_ENDPOINT(&r->listen),
			"err", _LOGV(evutil_socket_error_to_string(err)));
		evutil_closesocket(fd);
		return rc;
	}

	if (listen(fd, -1) < 0) {
		int err = EVUTIL_SOCKET_ERROR();

		LOG_ERROR("failed to listen on tcp listener socket",
			"listen", _LOGV_ENDPOINT(&r->listen),
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

		LOG_ERROR("failed to create tcp listener",
			"listen", _LOGV_ENDPOINT(&r->listen),
			"err", _LOGV(evutil_socket_error_to_string(err)));
		evutil_closesocket(fd);
		return err ? -err : -EIO;
	}

	ctx->listener = listener;
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
	case ENDPOINT_INET6:
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
