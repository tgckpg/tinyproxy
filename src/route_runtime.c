#include <string.h>

#include "klog.h"
#include "route.h"
#include "route_runtime.h"

int start_route(struct worker *w, const struct route *r, struct route_ctx *ctx)
{
	int rc = 0;
	switch (r->listen.proto) {

	case PROTO_TCP:
		ctx->kind = ROUTE_CTX_STREAM;

		rc = start_stream_route(w, r, &ctx->u.stream);
		if (rc != 0) {
			ctx->kind = ROUTE_CTX_NONE;
			memset(&ctx->u.stream, 0, sizeof(ctx->u.stream));
		}

		return rc;

	case PROTO_UDP:
		ctx->kind = ROUTE_CTX_DATAGRAM;

		rc = start_datagram_route(w, r, &ctx->u.datagram);
		if (rc != 0) {
			ctx->kind = ROUTE_CTX_NONE;
			memset(&ctx->u.stream, 0, sizeof(ctx->u.datagram));
		}

		return rc;

	/* New approach
	case ENDPOINT_PROTO_TCP:
	case ENDPOINT_PROTO_UNIX_STREAM:
		return start_stream_listener(w, r, ctx);

	case ENDPOINT_PROTO_UDP:
	case ENDPOINT_PROTO_UNIX_DGRAM:
		return start_datagram_listener(w, r, ctx);
	*/

	default:
		LOG_ERROR("route listen endpoint is not listenable",
			"line", _LOGV(r->line_no),
			"listen", _LOGV_ENDPOINT(&r->listen)
		);
		return -EINVAL;
	}
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
