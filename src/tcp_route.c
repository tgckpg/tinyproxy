#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/util.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "klog.h"
#include "file_conf.h"
#include "compat_socket.h"
#include "proxy_proto_v2.h"
#include "route.h"
#include "tcp_route.h"
#include "x_builtins.h"

#define BEV_READ_HIGH_WATER (256 * 1024)
#define BEV_WRITE_RESUME_WATER (128 * 1024)

#define FILE_CHUNK_SIZE 4096

struct tcp_route_ctx {
	struct event_base *accept_base;
	struct worker *worker;
	const struct route *route;
	struct evconnlistener *listener;
};

typedef struct conn_s {
	struct worker *owner;
	const struct route *route;

	struct bufferevent *client;
	struct bufferevent *upstream;

	struct sockaddr_storage peer_addr;
	socklen_t peer_addr_len;
} conn_t;

struct file_conn {
	conn_t *conn;
	FILE *fp;
};

struct accepted_client {
	evutil_socket_t fd;
	struct sockaddr_storage peer_addr;
	socklen_t peer_addr_len;
	const struct route *route;
};

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

static void pipe_read_cb(struct bufferevent *src, void *arg)
{
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

	if (evbuffer_get_length(output) >= BEV_READ_HIGH_WATER) {
		bufferevent_disable(src, EV_READ);
	}
}

static void pipe_write_cb(struct bufferevent *dst, void *arg)
{
	conn_t *conn = arg;
	struct bufferevent *src;

	if (dst == conn->client) {
		src = conn->upstream;
	} else if (dst == conn->upstream) {
		src = conn->client;
	} else {
		return;
	}

	struct evbuffer *output = bufferevent_get_output(dst);

	if (evbuffer_get_length(output) < BEV_WRITE_RESUME_WATER) {
		bufferevent_enable(src, EV_READ);
	}
}

static void set_connect_timeout(conn_t *conn, const struct route *r)
{
	struct timeval connect_timeout = {
		.tv_sec = r->opts.connect_timeout_sec,
		.tv_usec = 0,
	};

	bufferevent_set_timeouts(conn->upstream, NULL, &connect_timeout);
}

static void set_idle_timeouts(conn_t *conn, const struct route *r)
{
	struct timeval idle_timeout = {
		.tv_sec = r->opts.idle_timeout_sec,
		.tv_usec = 0,
	};

	bufferevent_set_timeouts(conn->client, &idle_timeout, &idle_timeout);
	bufferevent_set_timeouts(conn->upstream, &idle_timeout, &idle_timeout);
}

static void set_client_idle_timeout(conn_t *conn, const struct route *r)
{
	struct timeval idle_timeout = {
		.tv_sec = r->opts.idle_timeout_sec,
		.tv_usec = 0,
	};

	bufferevent_set_timeouts(conn->client, &idle_timeout, &idle_timeout);
}

static int set_socket_keepalive(evutil_socket_t fd, const struct route *r)
{
	int v = r->opts.keep_alive ? 1 : 0;

	if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&v, sizeof(v)) < 0) {
		return -errno;
	}

	return 0;
}

static void event_cb(struct bufferevent *bev, short events, void *arg) {
	conn_t *conn = arg;

	if (events & BEV_EVENT_CONNECTED) {
		set_idle_timeouts(conn, conn->route);

		bufferevent_enable(conn->client, EV_READ | EV_WRITE);
		bufferevent_enable(conn->upstream, EV_READ | EV_WRITE);
		return;
	}

	if (events & BEV_EVENT_TIMEOUT) {
		LOG_WARN("connection timed out");
		free_conn(conn);
		return;
	}

	if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
		if (events & BEV_EVENT_ERROR) {
			int err = EVUTIL_SOCKET_ERROR();
			LOG_ERROR("connection error",
				"err", _LOGV(evutil_socket_error_to_string(err))
			);
		}

		free_conn(conn);
	}

	(void)bev;
}

static int connect_upstream(struct bufferevent *bev, const struct endpoint *ep)
{
	if (ep == NULL) {
		return -EINVAL;
	}

	switch (ep->kind) {
	case ENDPOINT_INET: {
		struct sockaddr_in addr;

		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(ep->port);

		if (inet_pton(AF_INET, ep->host, &addr.sin_addr) != 1) {
			return -EINVAL;
		}

		if (bufferevent_socket_connect(
			    bev,
			    (struct sockaddr *)&addr,
			    sizeof(addr)) < 0) {
			return -errno;
		}

		return 0;
	}

	case ENDPOINT_UNIX:
#ifdef _WIN32
		return -ENOTSUP;
#else
	{
		struct sockaddr_un addr;

		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;

		if (ep->path[0] == '\0') {
			return -EINVAL;
		}

		if (strlen(ep->path) >= sizeof(addr.sun_path)) {
			return -ENAMETOOLONG;
		}

		strcpy(addr.sun_path, ep->path);

		if (bufferevent_socket_connect(
			    bev,
			    (struct sockaddr *)&addr,
			    sizeof(addr)) < 0) {
			return -errno;
		}

		return 0;
	}
#endif

	default:
		return -ENOTSUP;
	}
}

static const char *tcp_client_addr_string(conn_t *conn, char *buf, size_t buf_len)
{
	struct sockaddr_storage ss;
	socklen_t len = sizeof(ss);
	evutil_socket_t fd;
	int port = 0;

	if (conn == NULL || conn->client == NULL || buf == NULL || buf_len == 0) {
		return NULL;
	}

	fd = bufferevent_getfd(conn->client);
	if (fd < 0) {
		return NULL;
	}

	if (getpeername(fd, (struct sockaddr *)&ss, &len) < 0) {
		return NULL;
	}

	if (ss.ss_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&ss;

		if (inet_ntop(AF_INET, &sin->sin_addr, buf, buf_len) == NULL) {
			return NULL;
		}

		port = ntohs(sin->sin_port);
	} else if (ss.ss_family == AF_INET6) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;

		if (inet_ntop(AF_INET6, &sin6->sin6_addr, buf, buf_len) == NULL) {
			return NULL;
		}

		port = ntohs(sin6->sin6_port);
	} else {
		snprintf(buf, buf_len, "unknown");
		return buf;
	}

	snprintf(buf + strlen(buf), buf_len - strlen(buf), ":%d", port);
	return buf;
}

static void builtin_client_read_cb(struct bufferevent *bev, void *arg)
{
	struct evbuffer *input = bufferevent_get_input(bev);

	(void)arg;

	evbuffer_drain(input, evbuffer_get_length(input));
}

static void builtin_close_after_write_cb(struct bufferevent *bev, void *arg)
{
	conn_t *conn = arg;

	if (evbuffer_get_length(bufferevent_get_output(bev)) == 0) {
		free_conn(conn);
	}
}

static void file_done(struct file_conn *fc)
{
	if (fc == NULL) {
		return;
	}

	if (fc->fp != NULL) {
		fclose(fc->fp);
		fc->fp = NULL;
	}

	free_conn(fc->conn);
	free(fc);
}

static void file_event_cb(struct bufferevent *bev, short events, void *arg)
{
	struct file_conn *fc = arg;

	(void)bev;
	(void)events;

	file_done(fc);
}

static void file_write_cb(struct bufferevent *bev, void *arg)
{
	struct file_conn *fc = arg;
	struct evbuffer *out = bufferevent_get_output(bev);
	char buf[FILE_CHUNK_SIZE];

	while (evbuffer_get_length(out) < BEV_READ_HIGH_WATER) {
		size_t n = fread(buf, 1, sizeof(buf), fc->fp);

		if (n > 0) {
			if (bufferevent_write(bev, buf, n) < 0) {
				file_done(fc);
				return;
			}

			continue;
		}

		if (ferror(fc->fp)) {
			file_done(fc);
			return;
		}

		/*
		 * EOF. If nothing is queued anymore, close now.
		 * Otherwise keep the write callback installed so we close
		 * after libevent drains the remaining output.
		 */
		if (evbuffer_get_length(out) == 0) {
			file_done(fc);
		}

		return;
	}
}

static int start_tcp_file(conn_t *conn)
{
	const struct route *r;
	struct file_conn *fc;
	FILE *fp;

	if (conn == NULL || conn->route == NULL) {
		return -EINVAL;
	}

	r = conn->route;

	fp = fopen(r->upstream.path, "rb");
	if (fp == NULL) {
		int err = errno;

		LOG_ERROR("failed to open file upstream",
			"path", _LOGV(r->upstream.path),
			"err", _LOGV(strerror(err))
		);

		free_conn(conn);
		return -err;
	}

	if (conn->upstream != NULL) {
		bufferevent_free(conn->upstream);
		conn->upstream = NULL;
	}

	fc = calloc(1, sizeof(*fc));
	if (fc == NULL) {
		fclose(fp);
		free_conn(conn);
		return -ENOMEM;
	}

	fc->conn = conn;
	fc->fp = fp;

	bufferevent_setwatermark(conn->client, EV_WRITE, 0, BEV_READ_HIGH_WATER);
	bufferevent_setcb(conn->client, NULL, file_write_cb, file_event_cb, fc);
	bufferevent_enable(conn->client, EV_WRITE);

	set_client_idle_timeout(conn, r);

	file_write_cb(conn->client, fc);

	return 0;
}

static int start_tcp_builtin(conn_t *conn)
{
	struct x_builtin_request req;
	struct x_builtin_response res;
	const struct route *r;
	char client_addr[128];
	int rc;

	if (conn == NULL || conn->route == NULL) {
		return -EINVAL;
	}

	r = conn->route;

	memset(&req, 0, sizeof(req));

	req.builtin = r->upstream.builtin;
	req.client_addr = tcp_client_addr_string(conn, client_addr, sizeof(client_addr));

	rc = x_builtin_handle(&req, &res);
	if (rc < 0) {
		free_conn(conn);
		return rc;
	}

	if (conn->upstream != NULL) {
		bufferevent_free(conn->upstream);
		conn->upstream = NULL;
	}

	set_client_idle_timeout(conn, r);

	switch (res.action) {
	case X_BUILTIN_ACTION_CLOSE:
		if (res.data_len > 0) {
			bufferevent_write(conn->client, res.data, res.data_len);
			bufferevent_setcb(conn->client, NULL, builtin_close_after_write_cb, event_cb, conn);
			bufferevent_enable(conn->client, EV_WRITE);
		} else {
			free_conn(conn);
		}
		return 0;

	case X_BUILTIN_ACTION_DISCARD:
		bufferevent_setcb(conn->client, builtin_client_read_cb, NULL, event_cb, conn);
		bufferevent_enable(conn->client, EV_READ);
		return 0;

	case X_BUILTIN_ACTION_HANG:
		bufferevent_setcb(conn->client, NULL, NULL, event_cb, conn);
		bufferevent_enable(conn->client, EV_READ);
		return 0;

	default:
		free_conn(conn);
		return -EINVAL;
	}
}

static void worker_adopt_client_fd(struct worker *w, struct accepted_client *ac) {
	conn_t *conn = calloc(1, sizeof(*conn));
	if (conn == NULL) {
		evutil_closesocket(ac->fd);
		return;
	}

	const struct route *r = ac->route;

	conn->owner = w;
	conn->route = r;

	conn->peer_addr = ac->peer_addr;
	conn->peer_addr_len = ac->peer_addr_len;

	conn->client = bufferevent_socket_new(w->base, ac->fd, BEV_OPT_CLOSE_ON_FREE);
	if (conn->client == NULL) {
		free(conn);
		evutil_closesocket(ac->fd);
		return;
	}

	if (ac->route->upstream.kind == ENDPOINT_BUILTIN) {
		start_tcp_builtin(conn);
		return;
	}

	if (ac->route->upstream.kind == ENDPOINT_FILE) {
		start_tcp_file(conn);
		return;
	}

	conn->upstream = bufferevent_socket_new(w->base, -1, BEV_OPT_CLOSE_ON_FREE);
	if (conn->upstream == NULL) {
		free_conn(conn);
		return;
	}

	int rc = set_socket_keepalive(ac->fd, r);
	if (rc < 0) {
		LOG_WARN("failed to enable client TCP keepalive",
			"err", _LOGV(strerror(-rc))
		);
	}

	bufferevent_setwatermark(conn->client, EV_READ, 0, BEV_READ_HIGH_WATER);
	bufferevent_setwatermark(conn->upstream, EV_READ, 0, BEV_READ_HIGH_WATER);

	bufferevent_setcb(conn->client, pipe_read_cb, pipe_write_cb, event_cb, conn);
	bufferevent_setcb(conn->upstream, pipe_read_cb, pipe_write_cb, event_cb, conn);

	/*
	 * Do not read from the client yet.
	 *
	 * Otherwise client bytes may be copied into the upstream output buffer
	 * before the PROXY v2 header is queued.
	 */
	bufferevent_disable(conn->client, EV_READ);

	set_connect_timeout(conn, r);

	rc = connect_upstream(conn->upstream, &r->upstream);
	if (rc < 0) {
		LOG_ERROR("upstream connect failed",
			"upstream", _LOGV_ENDPOINT(&r->upstream),
			"err", _LOGV(strerror(-rc))
		);
		free_conn(conn);
		return;
	}

	if (r->opts.keep_alive && r->upstream.kind == ENDPOINT_INET) {
		evutil_socket_t upstream_fd = bufferevent_getfd(conn->upstream);

		if (upstream_fd >= 0) {
			int rc = set_socket_keepalive(upstream_fd, r);
			if (rc < 0) {
				LOG_WARN("failed to enable upstream TCP keepalive",
					"upstream", _LOGV_ENDPOINT(&r->upstream),
					"err", _LOGV(strerror(-rc))
				);
			}
		}
	}

	if (r->opts.proxy_v2) {
		struct sockaddr_in local_addr;
		socklen_t local_len = sizeof(local_addr);

		memset(&local_addr, 0, sizeof(local_addr));

		if (getsockname(ac->fd, (struct sockaddr *)&local_addr, &local_len) < 0) {
			perror("getsockname");
			free_conn(conn);
			return;
		}

		if (ac->peer_addr_len <= 0) {
			LOG_ERROR("invalid client address");
			free_conn(conn);
			return;
		}

		if (ac->peer_addr.ss_family != AF_INET ||
			local_addr.sin_family != AF_INET) {
			LOG_ERROR("PROXY v2 currently only supports IPv4 TCP");
			free_conn(conn);
			return;
		}

		unsigned char hdr[256];
		size_t hdr_len = 0;

		int rc = proxy_v2_build(
			hdr,
			sizeof(hdr),
			(const struct sockaddr *)&ac->peer_addr,
			ac->peer_addr_len,
			(const struct sockaddr *)&local_addr,
			local_len,
			SOCK_STREAM,
			&hdr_len
		);

		if (rc == 0) {
			rc = bufferevent_write(conn->upstream, hdr, hdr_len);
			if (rc < 0) {
				rc = -EIO;
			}
		}

		if (rc < 0) {
			LOG_ERROR("failed to write PROXY v2 header", "err", _LOGV(strerror(-rc)));
			free_conn(conn);
			return;
		}
	}

	bufferevent_enable(conn->client, EV_READ | EV_WRITE);
	bufferevent_enable(conn->upstream, EV_READ | EV_WRITE);
}

static void dispatch_client_fd(struct worker *w, struct accepted_client *ac) {
	worker_adopt_client_fd(w, ac);
}

static void accept_cb(
	struct evconnlistener *listener,
	evutil_socket_t client_fd,
	struct sockaddr *addr,
	int socklen,
	void *arg
) {
	(void)listener;

	struct tcp_route_ctx *ctx = arg;
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

static void accept_error_cb(struct evconnlistener *listener, void *arg)
{
	struct tcp_route_ctx *ctx = arg;
	int err = EVUTIL_SOCKET_ERROR();

	LOG_ERROR("accept error", "err", _LOGV(evutil_socket_error_to_string(err)));

	evconnlistener_disable(listener);
	event_base_loopexit(ctx->accept_base, NULL);
}

int start_tcp_route(
	struct worker *w,
	const struct route *r,
	struct tcp_route_ctx **out)
{
	struct sockaddr_in listen_addr;
	memset(&listen_addr, 0, sizeof(listen_addr));
	listen_addr.sin_family = AF_INET;
	listen_addr.sin_port = htons(r->listen.port);

	if (inet_pton(AF_INET, r->listen.host, &listen_addr.sin_addr) != 1) {
		LOG_ERROR("invalid listen address",
			"listen", _LOGV_ENDPOINT(&r->listen)
		);
		return -EINVAL;
	}

	struct tcp_route_ctx *ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->accept_base = w->base;
	ctx->worker = w;
	ctx->route = r;

	ctx->listener = evconnlistener_new_bind(
		ctx->accept_base,
		accept_cb,
		ctx,
		LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
		128,
		(struct sockaddr *)&listen_addr,
		sizeof(listen_addr)
	);

	if (ctx->listener == NULL) {
		LOG_ERROR("evconnlistener_new_bind failed");
		free(ctx);
		return -EADDRINUSE;
	}

	evconnlistener_set_error_cb(ctx->listener, accept_error_cb);

	char opts[128];

	route_options_str(&r->opts, opts, sizeof(opts));

	LOG_INFO("route started",
		"line", _LOGV(r->line_no),
		"listen", _LOGV_ENDPOINT(&r->listen),
		"upstream", _LOGV_ENDPOINT(&r->upstream),
		"options", _LOGV(opts[0] ? opts : "")
	);

	*out = ctx;
	return 0;
}

void free_tcp_route(struct tcp_route_ctx *ctx)
{
	if (ctx == NULL) {
		return;
	}

	if (ctx->listener != NULL) {
		evconnlistener_free(ctx->listener);
	}

	free(ctx);
}

#ifdef FUZZ
int tcp_route_adopt_client_for_fuzz(
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
