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
#include "stream_pipe.h"
#include "stream_sniff.h"
#include "proxy_proto_v2.h"

#include <event2/bufferevent.h>
#include <event2/buffer.h>

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
		finish_client_write(conn);
		conn->close_client_after_drain = false;
		conn->close_after_client_eof = true;
	}
}

/*
 * Finish the server/client response side without aborting the TCP connection.
 *
 * An empty libevent output buffer only means libevent handed the bytes to the
 * kernel. It does not prove the peer application has read the response yet.
 * Under high connection churn, immediately freeing/closing the socket after
 * output drain can still be observed by strict clients as a TCP reset.
 *
 * Half-close the write side to signal that no more bytes will be sent, then
 * keep EV_READ enabled so the peer can close, reset, or hit the idle timeout.
 *
 * This is TCP shutdown robustness, not an HTTP protocol requirement.
 */
void finish_client_write(conn_t *conn)
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
	 * Keep reading briefly after half-closing the write side so the peer can
	 * close cleanly. Do not wait for the full idle timeout here; otherwise a
	 * client that never sends EOF/RST can pin this connection too long.
	 */
	bufferevent_enable(conn->client, EV_READ);

	struct timeval close_wait_timeout = {
		.tv_sec = CLOSE_WAIT_TIMEOUT_SEC,
		.tv_usec = 0,
	};

	bufferevent_set_timeouts(conn->client, &close_wait_timeout, NULL);
}

void stream_client_event_cb(struct bufferevent *bev, short events, void *arg)
{
	(void)bev;

	conn_t *conn = arg;
	const struct route *r = conn->route;

	if (events & BEV_EVENT_TIMEOUT) {
		LOG_DEBUG("client connection timed out",
			"listen", _LOGV_ENDPOINT(&r->listen),
			"upstream", _LOGV_ENDPOINT(&r->upstream)
		);
		goto out_free;
	}

	if (events & BEV_EVENT_ERROR) {
		int err = EVUTIL_SOCKET_ERROR();

		if (conn->close_after_client_eof && err == ECONNRESET) {
			LOG_DEBUG("client reset after response drain",
				"listen", _LOGV_ENDPOINT(&r->listen),
				"upstream", _LOGV_ENDPOINT(&r->upstream)
			);
			goto out_free;
		}

		if (err == ECONNRESET) {
			LOG_DEBUG("client reset connection",
				"listen", _LOGV_ENDPOINT(&r->listen),
				"upstream", _LOGV_ENDPOINT(&r->upstream)
			);
			goto out_free;
		}

#ifdef EPIPE
		if (err == EPIPE) {
			LOG_DEBUG("client pipe closed",
				"listen", _LOGV_ENDPOINT(&r->listen),
				"upstream", _LOGV_ENDPOINT(&r->upstream)
			);
			goto out_free;
		}
#endif

#ifdef ETIMEDOUT
		if (err == ETIMEDOUT) {
			LOG_DEBUG("client connection timed out",
				"listen", _LOGV_ENDPOINT(&r->listen),
				"upstream", _LOGV_ENDPOINT(&r->upstream)
			);
			goto out_free;
		}
#endif

		LOG_WARN("client connection error",
			"err", _LOGV(evutil_socket_error_to_string(err)),
			"listen", _LOGV_ENDPOINT(&r->listen),
			"upstream", _LOGV_ENDPOINT(&r->upstream)
		);
		goto out_free;
	}

	if (events & BEV_EVENT_EOF) {
		goto out_free;
	}

	return;

out_free:
	free_conn(conn);
}

void stream_upstream_event_cb(struct bufferevent *bev, short events, void *arg)
{
	(void)bev;

	conn_t *conn = arg;
	const struct route *r = conn->route;

	char peer_buf[128];

	if (events & BEV_EVENT_CONNECTED) {
		conn->upstream_connected = true;
		set_idle_timeouts(conn, conn->route);

		bufferevent_enable(conn->client, EV_READ | EV_WRITE);
		return;
	}

	if (events & BEV_EVENT_TIMEOUT) {

		if (!conn->upstream_connected) {
			LOG_WARN("upstream connect timed out",
				"listen", _LOGV_ENDPOINT(&r->listen),
				"upstream", _LOGV_ENDPOINT(&r->upstream),
				"client_addr", _LOGV_SOCKADDR(&conn->peer_addr, conn->peer_addr_len,
					peer_buf, sizeof(peer_buf)),
				"client_sni", _LOGV(r->opts.sni_sniff ? stream_sniff_log_sni(&conn->sniff) : "")
			);
			goto out_free;
		}

		if (!client_has_pending_output(conn) && bev_output_len(conn->upstream) == 0) {
			LOG_DEBUG("stream idle timed out",
				"listen", _LOGV_ENDPOINT(&r->listen),
				"upstream", _LOGV_ENDPOINT(&r->upstream),
				"client_addr", _LOGV_SOCKADDR(&conn->peer_addr, conn->peer_addr_len,
					peer_buf, sizeof(peer_buf)),
				"client_sni", _LOGV(r->opts.sni_sniff ? stream_sniff_log_sni(&conn->sniff) : "")
			);
			goto out_free;
		}

		LOG_WARN("stream I/O stalled timed out",
			"client_output", _LOGV(bev_output_len(conn->client)),
			"upstream_output", _LOGV(bev_output_len(conn->upstream)),
			"listen", _LOGV_ENDPOINT(&r->listen),
			"upstream", _LOGV_ENDPOINT(&r->upstream),
			"client_addr", _LOGV_SOCKADDR(&conn->peer_addr, conn->peer_addr_len,
				peer_buf, sizeof(peer_buf)),
			"client_sni", _LOGV(r->opts.sni_sniff ? stream_sniff_log_sni(&conn->sniff) : "")
		);

		goto out_free;
	}

	if (events & BEV_EVENT_ERROR) {
		int err = EVUTIL_SOCKET_ERROR();

		if (client_has_pending_output(conn)) {
			LOG_WARN("upstream error after response queued; draining client",
				"err", _LOGV(evutil_socket_error_to_string(err)),
				"client_output", _LOGV(bev_output_len(conn->client)),
				"upstream_output", _LOGV(bev_output_len(conn->upstream)),
				"listen", _LOGV_ENDPOINT(&r->listen),
				"upstream", _LOGV_ENDPOINT(&r->upstream),
				"client_addr", _LOGV_SOCKADDR(&conn->peer_addr, conn->peer_addr_len,
					peer_buf, sizeof(peer_buf)),
				"client_sni", _LOGV(r->opts.sni_sniff ? stream_sniff_log_sni(&conn->sniff) : "")
			);
			goto out_drain_client;
		}

		if (err == ECONNRESET) {
			LOG_DEBUG("upstream reset connection",
				"listen", _LOGV_ENDPOINT(&r->listen),
				"upstream", _LOGV_ENDPOINT(&r->upstream),
				"client_addr", _LOGV_SOCKADDR(&conn->peer_addr, conn->peer_addr_len,
					peer_buf, sizeof(peer_buf)),
				"client_sni", _LOGV(r->opts.sni_sniff ? stream_sniff_log_sni(&conn->sniff) : "")
			);
			goto out_free;
		}

		LOG_WARN("upstream connection error",
			"err", _LOGV(evutil_socket_error_to_string(err)),
			"listen", _LOGV_ENDPOINT(&r->listen),
			"upstream", _LOGV_ENDPOINT(&r->upstream),
			"client_addr", _LOGV_SOCKADDR(&conn->peer_addr, conn->peer_addr_len,
				peer_buf, sizeof(peer_buf)),
			"client_sni", _LOGV(r->opts.sni_sniff ? stream_sniff_log_sni(&conn->sniff) : "")
		);
		goto out_free;
	}

	if (events & BEV_EVENT_EOF) {
		goto out_drain_client;
	}

	LOG_DEBUG("unhandled upstream event",
		"events", _LOGV(events),
		"listen", _LOGV_ENDPOINT(&r->listen),
		"upstream", _LOGV_ENDPOINT(&r->upstream)
	);

out_free:
	free_conn(conn);
	return;

out_drain_client:
	drain_client_then_close(conn);
}

static int connect_upstream(struct bufferevent *bev, const struct endpoint *ep)
{
	if (bev == NULL || ep == NULL) {
		return -EINVAL;
	}

	switch (ep->kind) {
	case ENDPOINT_INET:
	case ENDPOINT_INET6:
	{
		struct sockaddr_storage addr;
		socklen_t addr_len;
		int rc;

		rc = endpoint_to_sockaddr(ep, &addr, &addr_len);
		if (rc != 0) {
			return rc;
		}

		if (bufferevent_socket_connect(
				bev,
				(struct sockaddr *)&addr,
				addr_len) < 0) {
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

static void tune_stream_bev(struct bufferevent *bev)
{
	if (bev == NULL) {
		return;
	}

	bufferevent_set_max_single_read(bev, STREAM_IO_CHUNK_SIZE);
	bufferevent_set_max_single_write(bev, STREAM_IO_CHUNK_SIZE);
}

void worker_adopt_client_fd(struct worker *w, struct worker_stream_client_msg *ac) {
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

	tune_stream_bev(conn->client);
	tune_stream_bev(conn->upstream);

	bufferevent_setwatermark(conn->client, EV_READ, 0, STREAM_READ_HIGH_WATER);
	bufferevent_setwatermark(conn->upstream, EV_READ, 0, STREAM_READ_HIGH_WATER);

	bufferevent_setcb(conn->client, pipe_client_read_cb, pipe_client_write_cb, stream_client_event_cb, conn);
	bufferevent_setcb(conn->upstream, pipe_upstream_read_cb, pipe_upstream_write_cb, stream_upstream_event_cb, conn);

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

	/*
	 * Enable upstream writes so the async connect can complete.
	 * Client reads are enabled only after BEV_EVENT_CONNECTED.
	 */
	bufferevent_enable(conn->upstream, EV_READ | EV_WRITE);
}

int dispatch_client_fd(struct worker *w,
	const struct route *route,
	evutil_socket_t fd,
	const struct sockaddr *addr,
	socklen_t addr_len)
{
	return worker_enqueue_stream_client(w, route, fd, addr, addr_len);
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

	struct worker_stream_client_msg ac;
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
