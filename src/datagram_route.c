#include <event2/event.h>

#include <errno.h>
#include <string.h>

#include "klog.h"
#include "env.h"
#include "route.h"
#include "datagram_route.h"
#include "datagram_listener.h"
#include "datagram_client.h"

static int prepare_datagram_route(struct datagram_route_ctx *ctx)
{
	const struct route *r = ctx->route;

	if (r->upstream.kind == ENDPOINT_UNIX_DGRAM) {
		const char *runtime_dir = tinyproxy_runtime_dir();

		if (compat_mkdir(runtime_dir, 0755) < 0 && errno != EEXIST) {
			LOG_ERROR("failed to create unix-dgram runtime dir",
				"dir", _LOGV(runtime_dir),
				"err", _LOGV(strerror(errno))
			);
			return -errno;
		}
	}

	return 0;
}

int start_datagram_route(
	struct event_base *accept_base,
	struct worker_pool *wpool,
	const struct route *r,
	struct datagram_route_ctx *ctx)
{
	int rc;
	char opts[128];

	if (ctx == NULL) {
		return -EINVAL;
	}

	memset(ctx, 0, sizeof(*ctx));

	ctx->base = accept_base;
	ctx->worker_pool = wpool;
	ctx->route = r;
	ctx->listen_fd = EVUTIL_INVALID_SOCKET;

	rc = compat_mutex_init(&ctx->clients_mu);
	if (rc != 0) {
		memset(ctx, 0, sizeof(*ctx));
		ctx->listen_fd = EVUTIL_INVALID_SOCKET;
		return rc;
	}

	rc = prepare_datagram_route(ctx);
	if (rc != 0) {
		free_datagram_route(ctx);
		return rc;
	}

	rc = bind_datagram_listener(ctx);
	if (rc != 0) {
		free_datagram_route(ctx);
		return rc;
	}

	route_options_str(&r->opts, opts, sizeof(opts));

	LOG_INFO("datagram route started",
		"line", _LOGV(r->line_no),
		"listen", _LOGV_ENDPOINT(&r->listen),
		"upstream", _LOGV_ENDPOINT(&r->upstream),
		"options", _LOGV(opts[0] ? opts : "")
	);

	return 0;
}

#ifndef _WIN32
static void stop_unix_datagram_route(struct datagram_route_ctx *ctx)
{
	const struct route *r;
	const char *path;
	struct stat st;

	if (ctx == NULL || ctx->route == NULL) {
		return;
	}

	r = ctx->route;

	if (r->listen.kind != ENDPOINT_UNIX_DGRAM) {
		return;
	}

	path = r->listen.path;
	if (path[0] == '\0') {
		return;
	}

	if (lstat(path, &st) < 0) {
		if (errno != ENOENT) {
			LOG_WARN("failed to inspect unix datagram listener socket",
				"line", _LOGV(r->line_no),
				"path", _LOGV(path),
				"err", _LOGV(strerror(errno)));
		}
		return;
	}

	if (!S_ISSOCK(st.st_mode)) {
		LOG_WARN("not removing unix datagram listener path because it is not a socket",
			"line", _LOGV(r->line_no),
			"path", _LOGV(path));
		return;
	}

	if (unlink(path) < 0) {
		LOG_WARN("failed to remove unix datagram listener socket",
			"line", _LOGV(r->line_no),
			"path", _LOGV(path),
			"err", _LOGV(strerror(errno)));
	}
}
#endif

void stop_datagram_route_listener(struct datagram_route_ctx *ctx)
{
	if (ctx == NULL) {
		return;
	}

	if (ctx->listen_ev) {
		event_free(ctx->listen_ev);
		ctx->listen_ev = NULL;
	}

	/*
	 * Do NOT close ctx->listen_fd here.
	 * Workers may still use it to send UDP replies.
	 */
}

void free_datagram_route(struct datagram_route_ctx *ctx)
{
	struct datagram_client *c;

	if (!ctx) {
		return;
	}

	compat_mutex_lock(&ctx->clients_mu);

	while (ctx->clients) {
		c = ctx->clients;
		ctx->clients = c->next;
		c->next = NULL;
		free_datagram_client(c);
	}

	compat_mutex_unlock(&ctx->clients_mu);

	if (ctx->listen_ev) {
		event_free(ctx->listen_ev);
		ctx->listen_ev = NULL;
	}

	if (ctx->listen_fd != EVUTIL_INVALID_SOCKET) {
		evutil_closesocket(ctx->listen_fd);
		ctx->listen_fd = EVUTIL_INVALID_SOCKET;
	}

#ifndef _WIN32
	stop_unix_datagram_route(ctx);
#endif

	compat_mutex_destroy(&ctx->clients_mu);

	memset(ctx, 0, sizeof(*ctx));
	ctx->listen_fd = EVUTIL_INVALID_SOCKET;
}
