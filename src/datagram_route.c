#include <event2/event.h>
#include <event2/util.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "klog.h"
#include "env.h"
#include "file_conf.h"
#include "compat_socket.h"
#include "compat_file.h"
#include "proxy_proto_v2.h"
#include "worker.h"
#include "route.h"
#include "datagram_route.h"
#include "x_builtins.h"

#define UDP_MAX_PACKET 65535

struct datagram_route_ctx;

struct udp_client {
	struct datagram_route_ctx *ctx;

	evutil_socket_t fd;
	struct event *ev;

	char unix_local_path[108];

	struct sockaddr_storage client_addr;
	socklen_t client_addr_len;

	time_t last_seen;

	struct udp_client *next;
};

static int sockaddr_equal(
	const struct sockaddr_storage *a,
	socklen_t a_len,
	const struct sockaddr_storage *b,
	socklen_t b_len
) {
	if (a_len != b_len) {
		return 0;
	}

	if (a->ss_family != b->ss_family) {
		return 0;
	}

	return memcmp(a, b, (size_t)a_len) == 0;
}

static void free_udp_client(struct udp_client *c)
{
	if (c == NULL) {
		return;
	}

	if (c->ev != NULL) {
		event_free(c->ev);
	}

	if (c->fd >= 0) {
		evutil_closesocket(c->fd);
	}

	if (c->unix_local_path[0] != '\0') {
		unlink(c->unix_local_path);
	}

	free(c);
}

static void cleanup_idle_udp_clients(struct datagram_route_ctx *ctx)
{
	time_t now = time(NULL);
	struct udp_client **pp = &ctx->clients;

	while (*pp != NULL) {
		struct udp_client *c = *pp;

		if (now - c->last_seen <= ctx->route->opts.idle_timeout_sec) {
			pp = &c->next;
			continue;
		}

		*pp = c->next;
		c->next = NULL;

		LOG_INFO("udp client expired",
			"client_family", _LOGV(c->client_addr.ss_family),
			"client_len", _LOGV(c->client_addr_len)
		);

		free_udp_client(c);
	}
}

static struct udp_client *find_udp_client(
	struct datagram_route_ctx *ctx,
	const struct sockaddr_storage *addr,
	socklen_t addr_len
) {
	for (struct udp_client *c = ctx->clients; c != NULL; c = c->next) {
		if (sockaddr_equal(&c->client_addr, c->client_addr_len, addr, addr_len)) {
			return c;
		}
	}

	return NULL;
}

static void upstream_read_cb(evutil_socket_t fd, short events, void *arg)
{
	(void)events;

	struct udp_client *c = arg;
	struct datagram_route_ctx *ctx = c->ctx;

	unsigned char buf[UDP_MAX_PACKET];

	for (;;) {
		ssize_t n = recv(fd, (char *)buf, sizeof(buf), 0);
		if (n < 0) {
			int err = EVUTIL_SOCKET_ERROR();

			if (socket_err_is_retriable(err)) {
				return;
			}

			LOG_ERROR("udp recvfrom failed",
				"err", _LOGV(evutil_socket_error_to_string(err))
			);
			return;
		}

		if (n == 0) {
			return;
		}

		ssize_t sent = sendto(
			ctx->listen_fd,
			(const char *)buf,
			(size_t)n,
			0,
			(const struct sockaddr *)&c->client_addr,
			c->client_addr_len
		);

		if (sent < 0) {
			int err = EVUTIL_SOCKET_ERROR();

			LOG_ERROR("udp send to client failed",
				"err", _LOGV(evutil_socket_error_to_string(err))
			);
			return;
		}
	}
}

static const char *udp_client_addr_string(
	const struct udp_client *c,
	char *buf,
	size_t buf_len
) {
	void *addr;
	uint16_t port;

	if (c == NULL || buf == NULL || buf_len == 0) {
		return NULL;
	}

	switch (c->client_addr.ss_family) {
	case AF_INET: {
		const struct sockaddr_in *sin =
			(const struct sockaddr_in *)&c->client_addr;

		addr = (void *)&sin->sin_addr;
		port = ntohs(sin->sin_port);

		if (inet_ntop(AF_INET, addr, buf, buf_len) == NULL) {
			return NULL;
		}

		break;
	}

	case AF_INET6: {
		const struct sockaddr_in6 *sin6 =
			(const struct sockaddr_in6 *)&c->client_addr;

		addr = (void *)&sin6->sin6_addr;
		port = ntohs(sin6->sin6_port);

		if (inet_ntop(AF_INET6, addr, buf, buf_len) == NULL) {
			return NULL;
		}

		break;
	}

	default:
		if (snprintf(buf, buf_len, "unknown") < 0) {
			return NULL;
		}
		return buf;
	}

	if (snprintf(buf + strlen(buf), buf_len - strlen(buf), ":%u", port) < 0) {
		return NULL;
	}

	return buf;
}

static int start_udp_builtin(struct udp_client *c)
{
	struct x_builtin_request req;
	struct x_builtin_response res;
	char client_addr[128];
	int rc;

	if (c == NULL || c->ctx == NULL || c->ctx->route == NULL) {
		return -EINVAL;
	}

	memset(&req, 0, sizeof(req));

	req.builtin = c->ctx->route->upstream.builtin;
	req.client_addr = udp_client_addr_string(c, client_addr, sizeof(client_addr));

	rc = x_builtin_handle(&req, &res);
	if (rc < 0) {
		return rc;
	}

	switch (res.action) {
	case X_BUILTIN_ACTION_CLOSE:
		if (res.data_len == 0) {
			return 0;
		}

		if (sendto(
			    c->ctx->listen_fd,
			    res.data,
			    res.data_len,
			    0,
			    (const struct sockaddr *)&c->client_addr,
			    c->client_addr_len) < 0) {
			return -EVUTIL_SOCKET_ERROR();
		}

		return 0;

	case X_BUILTIN_ACTION_DISCARD:
		return 0;

	case X_BUILTIN_ACTION_HANG:
		return 0;

	default:
		return -EINVAL;
	}
}

static int connect_udp_upstream(struct udp_client *c, const struct endpoint *upstream)
{
	if (upstream == NULL) {
		return -EINVAL;
	}

	switch (upstream->kind) {

	case ENDPOINT_INET: {
		struct sockaddr_in upstream_addr;

		memset(&upstream_addr, 0, sizeof(upstream_addr));
		upstream_addr.sin_family = AF_INET;
		upstream_addr.sin_port = htons(upstream->port);

		if (inet_pton(AF_INET, upstream->host, &upstream_addr.sin_addr) != 1) {
			return -EINVAL;
		}

		c->fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (c->fd < 0) {
			return -EVUTIL_SOCKET_ERROR();
		}

		if (connect(
				c->fd,
				(const struct sockaddr *)&upstream_addr,
				sizeof(upstream_addr)
			) < 0) {
			return -EVUTIL_SOCKET_ERROR();
		}

		return 0;
	}

	case ENDPOINT_UNIX_DGRAM:
#ifdef _WIN32
		return -ENOTSUP;
#else
	{
		struct sockaddr_un local_addr;
		struct sockaddr_un upstream_addr;

		if (upstream->path[0] == '\0') {
			return -EINVAL;
		}

		if (strlen(upstream->path) >= sizeof(upstream_addr.sun_path)) {
			return -ENAMETOOLONG;
		}

		c->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
		if (c->fd < 0) {
			return -EVUTIL_SOCKET_ERROR();
		}

		memset(&local_addr, 0, sizeof(local_addr));
		local_addr.sun_family = AF_UNIX;

		// UNIX_DGRAM sock for reply
		const char *runtime_dir = tinyproxy_runtime_dir();

		int n = snprintf(
			c->unix_local_path,
			sizeof(c->unix_local_path),
			"%s/udp-%ld-%p.sock",
			runtime_dir,
			(long)getpid(),
			(void *)c
		);

		if (n < 0 || (size_t)n >= sizeof(c->unix_local_path)) {
			return -ENAMETOOLONG;
		}

		if (strlen(c->unix_local_path) >= sizeof(local_addr.sun_path)) {
			return -ENAMETOOLONG;
		}

		unlink(c->unix_local_path);
		strcpy(local_addr.sun_path, c->unix_local_path);

		if (bind(
				c->fd,
				(const struct sockaddr *)&local_addr,
				sizeof(local_addr)
			) < 0) {
			return -EVUTIL_SOCKET_ERROR();
		}

		memset(&upstream_addr, 0, sizeof(upstream_addr));
		upstream_addr.sun_family = AF_UNIX;
		strcpy(upstream_addr.sun_path, upstream->path);

		if (connect(
				c->fd,
				(const struct sockaddr *)&upstream_addr,
				sizeof(upstream_addr)
			) < 0) {
			return -EVUTIL_SOCKET_ERROR();
		}

		return 0;
	}
#endif

	default:
		return -ENOTSUP;
	}
}

static struct udp_client *create_udp_client(
	struct datagram_route_ctx *ctx,
	const struct sockaddr_storage *client_addr,
	socklen_t client_addr_len
) {
	const struct route *r = ctx->route;

	struct udp_client *c = calloc(1, sizeof(*c));
	if (c == NULL) {
		return NULL;
	}

	c->ctx = ctx;
	c->fd = -1;
	c->client_addr = *client_addr;
	c->client_addr_len = client_addr_len;
	c->last_seen = time(NULL);

	int rc = connect_udp_upstream(c, &r->upstream);
	if (rc < 0) {
		LOG_ERROR("udp upstream connect failed",
			"upstream", _LOGV_ENDPOINT(&r->upstream),
			"err", _LOGV(evutil_socket_error_to_string(-rc))
		);
		free_udp_client(c);
		return NULL;
	}

	if (evutil_make_socket_nonblocking(c->fd) < 0) {
		LOG_ERROR("evutil_make_socket_nonblocking failed");
		free_udp_client(c);
		return NULL;
	}

	c->ev = event_new(ctx->base, c->fd, EV_READ | EV_PERSIST, upstream_read_cb, c);
	if (c->ev == NULL) {
		LOG_ERROR("event_new failed for udp upstream");
		free_udp_client(c);
		return NULL;
	}

	if (event_add(c->ev, NULL) < 0) {
		LOG_ERROR("event_add failed for udp upstream");
		free_udp_client(c);
		return NULL;
	}

	c->next = ctx->clients;
	ctx->clients = c;

	LOG_INFO("udp client created",
		"client_family", _LOGV(c->client_addr.ss_family),
		"client_len", _LOGV(c->client_addr_len)
	);

	return c;
}

static int send_udp_payload_to_upstream(
	struct udp_client *c,
	const unsigned char *payload,
	size_t payload_len
) {
	struct datagram_route_ctx *ctx = c->ctx;
	const struct route *r = ctx->route;

	if (!r->opts.proxy_v2) {
		ssize_t sent = send(c->fd, (const char *)payload, payload_len, 0);
		if (sent < 0) {
			return -EVUTIL_SOCKET_ERROR();
		}

		return 0;
	}

	if (c->client_addr.ss_family != AF_INET ||
		ctx->local_addr.ss_family != AF_INET) {
		LOG_ERROR("PROXY v2 UDP currently only supports IPv4",
			"client_family", _LOGV(c->client_addr.ss_family),
			"local_family", _LOGV(ctx->local_addr.ss_family)
		);
		return -EAFNOSUPPORT;
	}

	unsigned char hdr[256];
	size_t hdr_len = 0;

	int rc = proxy_v2_build(
		hdr,
		sizeof(hdr),
		(const struct sockaddr *)&c->client_addr,
		c->client_addr_len,
		(const struct sockaddr *)&ctx->local_addr,
		ctx->local_addr_len,
		SOCK_DGRAM,
		&hdr_len
	);
	if (rc != 0) {
		return rc;
	}

	if (hdr_len + payload_len > UDP_MAX_PACKET) {
		return -EMSGSIZE;
	}

	unsigned char out[UDP_MAX_PACKET];

	memcpy(out, hdr, hdr_len);
	memcpy(out + hdr_len, payload, payload_len);

	ssize_t sent = send(c->fd, (const char *)out, hdr_len + payload_len, 0);
	if (sent < 0) {
		return -EVUTIL_SOCKET_ERROR();
	}

	return 0;
}

static void listen_read_cb(evutil_socket_t fd, short events, void *arg)
{
	(void)events;

	struct datagram_route_ctx *ctx = arg;

	cleanup_idle_udp_clients(ctx);

	for (;;) {
		unsigned char buf[UDP_MAX_PACKET];

		struct sockaddr_storage client_addr;
		socklen_t client_addr_len = sizeof(client_addr);

		memset(&client_addr, 0, sizeof(client_addr));

		ssize_t n = recvfrom(
			fd,
			(char *)buf,
			sizeof(buf),
			0,
			(struct sockaddr *)&client_addr,
			&client_addr_len
		);

		if (n < 0) {
			int err = EVUTIL_SOCKET_ERROR();

			if (socket_err_is_retriable(err)) {
				return;
			}

			LOG_ERROR("udp listen recv failed",
				"err", _LOGV(evutil_socket_error_to_string(err))
			);
			return;
		}

		if (n == 0) {
			continue;
		}

		if (ctx->route->upstream.kind == ENDPOINT_BUILTIN) {
			struct udp_client tmp;

			memset(&tmp, 0, sizeof(tmp));
			tmp.ctx = ctx;
			tmp.fd = -1;
			tmp.client_addr = client_addr;
			tmp.client_addr_len = client_addr_len;
			tmp.last_seen = time(NULL);

			int rc = start_udp_builtin(&tmp);
			if (rc < 0) {
				LOG_ERROR("udp builtin failed",
						"err", _LOGV(evutil_socket_error_to_string(-rc))
						);
				return;
			}

			continue;
		}

		struct udp_client *c = find_udp_client(ctx, &client_addr, client_addr_len);
		if (c == NULL) {
			c = create_udp_client(ctx, &client_addr, client_addr_len);
			if (c == NULL) {
				return;
			}
		}

		c->last_seen = time(NULL);

		int rc = send_udp_payload_to_upstream(c, buf, (size_t)n);
		if (rc < 0) {
			LOG_ERROR("udp send to upstream failed",
				"err", _LOGV(evutil_socket_error_to_string(-rc))
			);
			return;
		}
	}
}

int start_datagram_route(
	struct worker *w,
	const struct route *r,
	struct datagram_route_ctx *ctx
) {
	if (r->listen.kind != ENDPOINT_INET) {
		LOG_ERROR("unsupported udp listen endpoint",
			"listen", _LOGV_ENDPOINT(&r->listen)
		);
		return -ENOTSUP;
	}

	if (r->upstream.kind != ENDPOINT_INET &&
		r->upstream.kind != ENDPOINT_BUILTIN &&
		r->upstream.kind != ENDPOINT_UNIX_DGRAM) {
		LOG_ERROR("unsupported udp upstream endpoint",
			"upstream", _LOGV_ENDPOINT(&r->upstream)
		);
		return -ENOTSUP;
	}

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

	struct sockaddr_in listen_addr;
	memset(&listen_addr, 0, sizeof(listen_addr));

	listen_addr.sin_family = AF_INET;
	listen_addr.sin_port = htons(r->listen.port);

	if (inet_pton(AF_INET, r->listen.host, &listen_addr.sin_addr) != 1) {
		LOG_ERROR("invalid udp listen address",
			"listen", _LOGV_ENDPOINT(&r->listen)
		);
		return -EINVAL;
	}

	memset(ctx, 0, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->base = w->base;
	ctx->worker = w;
	ctx->route = r;
	ctx->listen_fd = -1;

	ctx->listen_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (ctx->listen_fd < 0) {
		int err = EVUTIL_SOCKET_ERROR();

		LOG_ERROR("udp listen socket failed",
			"err", _LOGV(evutil_socket_error_to_string(err))
		);
		memset(ctx, 0, sizeof(*ctx));
		return -errno;
	}

	evutil_make_socket_closeonexec(ctx->listen_fd);

	if (evutil_make_socket_nonblocking(ctx->listen_fd) < 0) {
		LOG_ERROR("evutil_make_socket_nonblocking failed");
		evutil_closesocket(ctx->listen_fd);
		free(ctx);
		return -EINVAL;
	}

	int one = 1;
	setsockopt(
		ctx->listen_fd,
		SOL_SOCKET,
		SO_REUSEADDR,
		(const char *)&one,
		sizeof(one)
	);

	if (bind(
			ctx->listen_fd,
			(const struct sockaddr *)&listen_addr,
			sizeof(listen_addr)
		) < 0) {
		int err = EVUTIL_SOCKET_ERROR();

		LOG_ERROR("udp bind failed",
			"err", _LOGV(evutil_socket_error_to_string(err))
		);
		evutil_closesocket(ctx->listen_fd);
		free(ctx);
		return -EADDRINUSE;
	}

	ctx->local_addr_len = sizeof(ctx->local_addr);
	memset(&ctx->local_addr, 0, sizeof(ctx->local_addr));

	if (getsockname(
			ctx->listen_fd,
			(struct sockaddr *)&ctx->local_addr,
			&ctx->local_addr_len
		) < 0) {
		int err = EVUTIL_SOCKET_ERROR();

		LOG_ERROR("udp getsockname failed",
			"err", _LOGV(evutil_socket_error_to_string(err))
		);
		evutil_closesocket(ctx->listen_fd);
		free(ctx);
		return -EINVAL;
	}

	ctx->listen_ev = event_new(
		ctx->base,
		ctx->listen_fd,
		EV_READ | EV_PERSIST,
		listen_read_cb,
		ctx
	);
	if (ctx->listen_ev == NULL) {
		LOG_ERROR("event_new failed for udp listener");
		evutil_closesocket(ctx->listen_fd);
		free(ctx);
		return -ENOMEM;
	}

	if (event_add(ctx->listen_ev, NULL) < 0) {
		LOG_ERROR("event_add failed for udp listener");
		event_free(ctx->listen_ev);
		evutil_closesocket(ctx->listen_fd);
		free(ctx);
		return -EINVAL;
	}

	char opts[128];

	route_options_str(&r->opts, opts, sizeof(opts));

	LOG_INFO("udp route started",
		"line", _LOGV(r->line_no),
		"listen", _LOGV_ENDPOINT(&r->listen),
		"upstream", _LOGV_ENDPOINT(&r->upstream),
		"options", _LOGV(opts[0] ? opts : "")
	);

	return 0;
}

void stop_datagram_route(struct datagram_route_ctx *ctx)
{
	if (ctx == NULL) {
		return;
	}

	if (ctx->listen_ev != NULL) {
		event_free(ctx->listen_ev);
	}

	if (ctx->listen_fd >= 0) {
		evutil_closesocket(ctx->listen_fd);
	}

	struct udp_client *c = ctx->clients;
	while (c != NULL) {
		struct udp_client *next = c->next;
		free_udp_client(c);
		c = next;
	}

	memset(ctx, 0, sizeof(*ctx));
}
