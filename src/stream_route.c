#include <stdlib.h>
#include <string.h>

#include "klog.h"
#include "stream_route.h"
#include "worker.h"
#include "route.h"
#include "stream_listener.h"
#include "compat_file.h"

int start_stream_route(struct worker *w, const struct route *r,
	struct stream_route_ctx *ctx)
{
	int rc;
	char opts[128];

	memset(ctx, 0, sizeof(*ctx));

	ctx->accept_base = w->base;
	ctx->worker = w;
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

#ifndef _WIN32
static void stop_unix_stream_route(struct stream_route_ctx *ctx)
{
	const struct route *r;
	const char *path;
	struct stat st;

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

	if (unlink(path) < 0) {
		LOG_WARN("failed to remove unix stream listener socket",
			"line", _LOGV(r->line_no),
			"path", _LOGV(path),
			"err", _LOGV(strerror(errno)));
	}
}
#endif

void stop_stream_route(struct stream_route_ctx *ctx)
{
	if (ctx == NULL) {
		return;
	}

	if (ctx->listener != NULL) {
		evconnlistener_free(ctx->listener);
	}

#ifndef _WIN32
	stop_unix_stream_route(ctx);
#endif

	memset(ctx, 0, sizeof(*ctx));
}

#ifdef FUZZ
int stream_route_adopt_client_for_fuzz(
	struct event_base *base,
	const struct route *r,
	evutil_socket_t client_fd,
	const struct sockaddr_storage *peer_addr,
	socklen_t peer_addr_len
) {
	if (base == NULL || r == NULL || client_fd < 0) {
		return -1;
	}

	struct worker w;
	memset(&w, 0, sizeof(w));

	w.base = base;
	w.id = 0;

	struct accepted_client ac;
	memset(&ac, 0, sizeof(ac));

	ac.fd = client_fd;
	ac.route = r;

	if (peer_addr != NULL &&
	    peer_addr_len > 0 &&
	    peer_addr_len <= sizeof(ac.peer_addr)) {
		memcpy(&ac.peer_addr, peer_addr, peer_addr_len);
		ac.peer_addr_len = peer_addr_len;
	}

	worker_adopt_client_fd(&w, &ac);

	return 0;
}
#endif
