#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/util.h>

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file_conf.h"
#include "proxy_proto_v2.h"
#include "tcp_route.h"

#define PROXY_V2_SIG "\r\n\r\n\0\r\nQUIT\n"

static void free_conn(conn_t *conn) {
	if (conn == NULL) {
		return;
	}

	if (conn->client != NULL) {
		bufferevent_free(conn->client);
	}

	if (conn->upstream != NULL) {
		bufferevent_free(conn->upstream);
	}

	free(conn);
}

static void pipe_read_cb(struct bufferevent *src, void *arg) {
	conn_t *conn = arg;

	struct bufferevent *dst;

	if (src == conn->client) {
		dst = conn->upstream;
	} else if (src == conn->upstream) {
		dst = conn->client;
	} else {
		return;
	}

	struct evbuffer *input = bufferevent_get_input(src);
	struct evbuffer *output = bufferevent_get_output(dst);

	evbuffer_add_buffer(output, input);
}

static void event_cb(struct bufferevent *bev, short events, void *arg) {
	conn_t *conn = arg;

	if (events & BEV_EVENT_CONNECTED) {
		bufferevent_enable(conn->client, EV_READ | EV_WRITE);
		bufferevent_enable(conn->upstream, EV_READ | EV_WRITE);
		return;
	}

	if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
		if (events & BEV_EVENT_ERROR) {
			int err = EVUTIL_SOCKET_ERROR();
			fprintf(stderr, "connection error: %s\n",
					evutil_socket_error_to_string(err));
		}

		free_conn(conn);
	}

	(void)bev;
}

static void accept_cb(
	struct evconnlistener *listener,
	evutil_socket_t client_fd,
	struct sockaddr *addr,
	int socklen,
	void *arg
) {
	(void)listener;

	struct listener_ctx *ctx = arg;
	const struct route *r = ctx->route;
	struct event_base *base = ctx->base;

	conn_t *conn = calloc(1, sizeof(*conn));
	if (conn == NULL) {
		evutil_closesocket(client_fd);
		return;
	}

	conn->client = bufferevent_socket_new(base, client_fd, BEV_OPT_CLOSE_ON_FREE);
	conn->upstream = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);

	if (conn->client == NULL || conn->upstream == NULL) {
		free_conn(conn);
		return;
	}

	bufferevent_setcb(
		conn->client,
		pipe_read_cb,
		NULL,
		event_cb,
		conn
	);

	bufferevent_setcb(
		conn->upstream,
		pipe_read_cb,
		NULL,
		event_cb,
		conn
	);

	struct sockaddr_in upstream_addr;
	memset(&upstream_addr, 0, sizeof(upstream_addr));
	upstream_addr.sin_family = AF_INET;
	upstream_addr.sin_port = htons(r->upstream_port);

	if (inet_pton(AF_INET, r->upstream_host, &upstream_addr.sin_addr) != 1) {
		fprintf(stderr, "invalid upstream address\n");
		free_conn(conn);
		return;
	}

	/*
	 * Do not read from the client yet.
	 *
	 * Otherwise client bytes may be copied into the upstream output buffer
	 * before the PROXY v2 header is queued.
	 */
	bufferevent_disable(conn->client, EV_READ);

	if (bufferevent_socket_connect(
			conn->upstream,
			(struct sockaddr *)&upstream_addr,
			sizeof(upstream_addr)
		) < 0) {
		fprintf(stderr, "upstream connect failed\n");
		free_conn(conn);
		return;
	}

	if (r->send_proxy_v2) {
		struct sockaddr_in local_addr;
		socklen_t local_len = sizeof(local_addr);

		memset(&local_addr, 0, sizeof(local_addr));

		if (getsockname(client_fd, (struct sockaddr *)&local_addr, &local_len) < 0) {
			perror("getsockname");
			free_conn(conn);
			return;
		}

		if (addr == NULL || socklen < (int)sizeof(struct sockaddr_in)) {
			fprintf(stderr, "invalid client address\n");
			free_conn(conn);
			return;
		}

		if (((struct sockaddr *)addr)->sa_family != AF_INET ||
		    local_addr.sin_family != AF_INET) {
			fprintf(stderr, "PROXY v2 currently only supports IPv4 TCP\n");
			free_conn(conn);
			return;
		}

		if (proxy_v2_write_bufferevent(
			conn->upstream,
			addr,
			(socklen_t)socklen,
			(struct sockaddr *)&local_addr,
			local_len,
			SOCK_STREAM
		) < 0) {
			fprintf(stderr, "failed to write PROXY v2 header\n");
			free_conn(conn);
			return;
		}
	}

	bufferevent_enable(conn->client, EV_READ | EV_WRITE);
	bufferevent_enable(conn->upstream, EV_READ | EV_WRITE);
}

static void accept_error_cb(struct evconnlistener *listener, void *arg) {
	struct event_base *base = arg;
	int err = EVUTIL_SOCKET_ERROR();

	fprintf(stderr, "accept error: %s\n",
			evutil_socket_error_to_string(err));

	evconnlistener_free(listener);
	event_base_loopexit(base, NULL);
}

int start_tcp_route(
    struct event_base *base,
    const struct route *r,
    struct listener_ctx **out)
{
    struct sockaddr_in listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(r->listen_port);

    if (inet_pton(AF_INET, r->listen_host, &listen_addr.sin_addr) != 1) {
        fprintf(stderr, "invalid listen address: %s\n", r->listen_host);
        return -EINVAL;
    }

    struct listener_ctx *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return -ENOMEM;
    }

    ctx->base = base;
    ctx->route = r;

    ctx->listener = evconnlistener_new_bind(
        base,
        accept_cb,
        ctx,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
        128,
        (struct sockaddr *)&listen_addr,
        sizeof(listen_addr)
    );

    if (ctx->listener == NULL) {
        fprintf(stderr, "evconnlistener_new_bind failed\n");
        free(ctx);
        return -EADDRINUSE;
    }

    evconnlistener_set_error_cb(ctx->listener, accept_error_cb);

    fprintf(stderr, "listening on %s:%u, forwarding to %s:%u\n",
            r->listen_host, r->listen_port,
            r->upstream_host, r->upstream_port);

    *out = ctx;
    return 0;
}

void free_tcp_route(struct listener_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->listener != NULL) {
        evconnlistener_free(ctx->listener);
    }

    free(ctx);
}
