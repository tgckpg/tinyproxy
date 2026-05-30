#include <string.h>

#include "klog.h"
#include "route.h"
#include "route_runtime.h"

int validate_route(const struct route *r)
{
	if (r == NULL) {
		return -EINVAL;
	}

	if (!endpoint_is_listenable(&r->listen)) {
		LOG_ERROR("route listen endpoint is not listenable",
			"line", _LOGV(r->line_no),
			"listen", _LOGV_ENDPOINT(&r->listen)
		);
		return -EINVAL;
	}

	if (endpoint_is_stream(&r->listen)) {
		if (endpoint_is_stream(&r->upstream) ||
		    r->upstream.proto == PROTO_FILE ||
		    r->upstream.proto == PROTO_BUILTIN) {
			return 0;
		}

		LOG_ERROR("unsupported stream route upstream",
			"line", _LOGV(r->line_no),
			"listen", _LOGV_ENDPOINT(&r->listen),
			"upstream", _LOGV_ENDPOINT(&r->upstream)
		);
		return -EINVAL;
	}

	if (endpoint_is_datagram(&r->listen)) {
		if (endpoint_is_datagram(&r->upstream) ||
		    r->upstream.proto == PROTO_BUILTIN) {
			return 0;
		}

		LOG_ERROR("unsupported datagram route upstream",
			"line", _LOGV(r->line_no),
			"listen", _LOGV_ENDPOINT(&r->listen),
			"upstream", _LOGV_ENDPOINT(&r->upstream)
		);
		return -EINVAL;
	}

	return -EINVAL;
}

int start_route(
		struct event_base *accept_base,
		struct worker_pool *wp,
		const struct route *r,
		struct route_ctx *ctx)
{
	int rc;

	memset(ctx, 0, sizeof(*ctx));
	ctx->route = r;
	ctx->kind = ROUTE_CTX_NONE;

	if (endpoint_is_stream(&r->listen)) {
		ctx->kind = ROUTE_CTX_STREAM;

		rc = start_stream_route(accept_base, wp, r, &ctx->u.stream);
		if (rc != 0) {
			ctx->kind = ROUTE_CTX_NONE;
			memset(&ctx->u.stream, 0, sizeof(ctx->u.stream));
		}

		return rc;
	}

	if (endpoint_is_datagram(&r->listen)) {
		ctx->kind = ROUTE_CTX_DATAGRAM;

		rc = start_datagram_route(accept_base, wp, r, &ctx->u.datagram);
		if (rc != 0) {
			ctx->kind = ROUTE_CTX_NONE;
			memset(&ctx->u.datagram, 0, sizeof(ctx->u.datagram));
		}

		return rc;
	}

	LOG_ERROR("route listen endpoint is not listenable",
		"line", _LOGV(r->line_no),
		"listen", _LOGV_ENDPOINT(&r->listen)
	);

	return -EINVAL;
}

void stop_route(struct route_ctx *ctx)
{
	if (ctx == NULL) {
		return;
	}

	switch (ctx->kind) {
	case ROUTE_CTX_STREAM:
		stop_stream_route(&ctx->u.stream);
		break;

	case ROUTE_CTX_DATAGRAM:
		stop_datagram_route(&ctx->u.datagram);
		break;

	case ROUTE_CTX_NONE:
	default:
		break;
	}

	memset(ctx, 0, sizeof(*ctx));
}
