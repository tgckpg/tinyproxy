#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "klog.h"
#include "stream_route.h"
#include "compat.h"
#include "worker.h"
#include "route.h"
#include "stream_listener.h"
#include "stream_sniff.h"

int start_stream_route(
		struct event_base *accept_base,
		struct worker_pool *wpool,
		const struct route *r,
		struct stream_route_ctx *ctx)
{
	int rc;
	char opts[128];

	memset(ctx, 0, sizeof(*ctx));

	ctx->accept_base = accept_base;
	ctx->worker_pool = wpool;
	ctx->route = r;

	rc = bind_stream_listener(ctx);
	if (rc != 0) {
		memset(ctx, 0, sizeof(*ctx));
		return rc;
	}

	evconnlistener_set_error_cb(ctx->listener, accept_error_cb);

	route_options_str(&r->opts, opts, sizeof(opts));

	LOG_INFO("route started",
		"line", _LOGV(r->line_no),
		"listen", _LOGV_ENDPOINT(&r->listen),
		"upstream", _LOGV_ENDPOINT(&r->upstream),
		"options", _LOGV(opts[0] ? opts : ""));

	return 0;
}

static void stop_unix_stream_route(struct stream_route_ctx *ctx)
{
	const struct route *r;
	const char *path;

	if (ctx == NULL || ctx->route == NULL) {
		return;
	}

	r = ctx->route;

	if (r->listen.kind != ENDPOINT_UNIX) {
		return;
	}

	path = r->listen.path;
	if (path[0] == '\0') {
		return;
	}

#ifndef _WIN32
	struct stat st;

	if (lstat(path, &st) < 0) {
		if (errno != ENOENT) {
			LOG_WARN("failed to inspect unix stream listener socket",
				"line", _LOGV(r->line_no),
				"path", _LOGV(path),
				"err", _LOGV(strerror(errno)));
		}
		return;
	}

	if (!S_ISSOCK(st.st_mode)) {
		LOG_WARN("not removing unix stream listener path because it is not a socket",
			"line", _LOGV(r->line_no),
			"path", _LOGV(path));
		return;
	}
#endif

	if (compat_unlink(path) < 0 && errno != ENOENT) {
		LOG_WARN("failed to remove unix stream listener socket",
			"line", _LOGV(r->line_no),
			"path", _LOGV(path),
			"err", _LOGV(strerror(errno)));
	}
}

void stop_stream_route_listener(struct stream_route_ctx *ctx)
{
	if (ctx->listener) {
		evconnlistener_free(ctx->listener);
		ctx->listener = NULL;
	}
}

void free_stream_route(struct stream_route_ctx *ctx)
{
	if (!ctx) {
		return;
	}

#ifndef _WIN32
	stop_unix_stream_route(ctx);
#endif

	memset(ctx, 0, sizeof(*ctx));
}
