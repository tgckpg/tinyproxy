#include <event2/buffer.h>
#include <event2/util.h>

#include <stdlib.h>
#include <string.h>

#include "klog.h"
#include "worker.h"
#include "route.h"
#include "stream_conn.h"
#include "stream_file.h"
#include "stream_builtin.h"
#include "proxy_proto_v2.h"

void free_conn(conn_t *conn) {
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

void set_client_idle_timeout(conn_t *conn, const struct route *r)
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

#ifdef TINYPROXY_DEBUG
static const char *bev_side(conn_t *conn, struct bufferevent *bev)
{
	if (bev == conn->client) {
		return "client";
	}
	if (bev == conn->upstream) {
		return "upstream";
	}
	return "unknown";
}
#endif

static size_t bev_output_len(struct bufferevent *bev)
{
	if (bev == NULL) {
		return 0;
	}

	return evbuffer_get_length(bufferevent_get_output(bev));
}

static bool client_has_pending_output(conn_t *conn)
{
	return conn->client != NULL && bev_output_len(conn->client) > 0;
}

static void drain_client_then_close(conn_t *conn)
{
	conn->close_client_after_drain = true;

	if (conn->upstream != NULL) {
		bufferevent_disable(conn->upstream, EV_READ | EV_WRITE);
	}

	if (!client_has_pending_output(conn)) {
		free_conn(conn);
	}
}

void event_cb(struct bufferevent *bev, short events, void *arg)
{
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

	if (events & BEV_EVENT_ERROR) {
		int err = EVUTIL_SOCKET_ERROR();

		if (bev == conn->client &&
			conn->close_after_client_eof &&
			err == ECONNRESET) {
			LOG_INFO("client reset after response drain");
			free_conn(conn);
			return;
		}

		LOG_ERROR("connection error",
			"err", _LOGV(evutil_socket_error_to_string(err))
		);

		if (bev == conn->upstream && client_has_pending_output(conn)) {
			const struct route *r = conn->route;

			LOG_WARN("upstream error after response queued; draining client",
				"err", _LOGV(evutil_socket_error_to_string(err)),
				"client_output", _LOGV(bev_output_len(conn->client)),
				"upstream_output", _LOGV(bev_output_len(conn->upstream)),
				"listen", _LOGV_ENDPOINT(&r->listen),
				"upstream", _LOGV_ENDPOINT(&r->upstream)
			);

			drain_client_then_close(conn);
			return;
		}

		free_conn(conn);
		return;
	}

	if (events & BEV_EVENT_EOF) {
		if (bev == conn->client && conn->close_after_client_eof) {
			free_conn(conn);
			return;
		}

		if (bev == conn->upstream && client_has_pending_output(conn)) {
			drain_client_then_close(conn);
			return;
		}

		free_conn(conn);
		return;
	}
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

	default:
		return -ENOTSUP;
	}
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

#ifdef TINYPROXY_DEBUG
	const struct route *r = conn->route;

	size_t input_len = evbuffer_get_length(input);
	size_t output_before = evbuffer_get_length(output);

	LOG_INFO("stream pipe read",
		"line", _LOGV(r->line_no),
		"from", _LOGV(bev_side(conn, src)),
		"to", _LOGV(bev_side(conn, dst)),
		"input_len", _LOGV(input_len),
		"dst_output_before", _LOGV(output_before)
	);
#endif

	evbuffer_add_buffer(output, input);

	size_t output_after = evbuffer_get_length(output);

#ifdef TINYPROXY_DEBUG
	LOG_INFO("stream pipe queued",
		"line", _LOGV(r->line_no),
		"from", _LOGV(bev_side(conn, src)),
		"to", _LOGV(bev_side(conn, dst)),
		"dst_output_after", _LOGV(output_after)
	);
#endif

	if (output_after >= BEV_READ_HIGH_WATER) {
#ifdef TINYPROXY_DEBUG
		LOG_INFO("stream pipe backpressure pause",
			"line", _LOGV(r->line_no),
			"paused", _LOGV(bev_side(conn, src)),
			"dst_output_len", _LOGV(output_after)
		);
#endif

		bufferevent_disable(src, EV_READ);
	}
}

static void finish_client_write(conn_t *conn)
{
	evutil_socket_t fd = bufferevent_getfd(conn->client);

	if (fd >= 0) {
#ifndef _WIN32
		shutdown(fd, SHUT_WR);
#else
		shutdown(fd, SD_SEND);
#endif
	}

	bufferevent_disable(conn->client, EV_WRITE);

	/*
	 * Keep EV_READ enabled so we can observe client EOF instead of
	 * closing with unread data and causing RST on some platforms.
	 */
	bufferevent_enable(conn->client, EV_READ);
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
	size_t output_len = evbuffer_get_length(output);

#ifdef TINYPROXY_DEBUG
	LOG_INFO("stream pipe write",
		"line", _LOGV(conn->route->line_no),
		"dst", _LOGV(bev_side(conn, dst)),
		"src", _LOGV(bev_side(conn, src)),
		"dst_output_len", _LOGV(output_len)
	);
#endif

	if (dst == conn->client &&
		conn->close_client_after_drain &&
		output_len == 0) {
		LOG_INFO("client output drained; shutting down write side",
			"line", _LOGV(conn->route->line_no)
		);

		finish_client_write(conn);
		conn->close_client_after_drain = false;
		conn->close_after_client_eof = true;
		return;
	}

	if (src != NULL && output_len < BEV_WRITE_RESUME_WATER) {
#ifdef TINYPROXY_DEBUG
		LOG_INFO("stream pipe backpressure resume",
			"line", _LOGV(conn->route->line_no),
			"resumed", _LOGV(bev_side(conn, src)),
			"dst", _LOGV(bev_side(conn, dst)),
			"dst_output_len", _LOGV(output_len)
		);
#endif

		bufferevent_enable(src, EV_READ);
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
		start_stream_builtin(conn);
		return;
	}

	if (ac->route->upstream.kind == ENDPOINT_FILE) {
		start_stream_file(conn);
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

void dispatch_client_fd(struct worker *w, struct accepted_client *ac) {
	worker_adopt_client_fd(w, ac);
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
