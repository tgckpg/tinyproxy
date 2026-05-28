#include <event2/event.h>
#include <event2/util.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "klog.h"
#include "env.h"
#include "file_conf.h"
#include "proxy_proto_v2.h"
#include "worker.h"
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
	struct worker *w,
	const struct route *r,
	struct datagram_route_ctx *ctx
) {
	int rc;
	char opts[128];

	if (ctx == NULL) {
		return -EINVAL;
	}

	memset(ctx, 0, sizeof(*ctx));

	ctx->base = w->base;
	ctx->worker = w;
	ctx->route = r;
	ctx->listen_fd = -1;

	rc = prepare_datagram_route(ctx);
	if (rc != 0) {
		memset(ctx, 0, sizeof(*ctx));
		return rc;
	}

	rc = bind_datagram_listener(ctx);
	if (rc != 0) {
		stop_datagram_route(ctx);
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

void stop_datagram_route(struct datagram_route_ctx *ctx)
{
	if (ctx == NULL) {
		return;
	}

	while (ctx->clients != NULL) {
		struct datagram_client *c = ctx->clients;
		ctx->clients = c->next;
		c->next = NULL;
		free_datagram_client(c);
	}

	if (ctx->listen_ev != NULL) {
		event_free(ctx->listen_ev);
		ctx->listen_ev = NULL;
	}

#ifndef _WIN32
	stop_unix_datagram_route(ctx);
#endif

	if (ctx->listen_fd >= 0) {
		evutil_closesocket(ctx->listen_fd);
		ctx->listen_fd = -1;
	}

	memset(ctx, 0, sizeof(*ctx));
	ctx->listen_fd = -1;
}
