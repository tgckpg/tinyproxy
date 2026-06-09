#include <event2/buffer.h>

#include "klog.h"
#include "route.h"
#include "stream_conn.h"
#include "stream_pipe.h"
#include "stream_sniff.h"

void pipe_client_read_cb(struct bufferevent *client, void *arg)
{
	conn_t *conn = arg;
	struct bufferevent *upstream = conn->upstream;
	struct evbuffer *src;
	struct evbuffer *dst;

	if (upstream == NULL) {
		return;
	}

	src = bufferevent_get_input(client);
	dst = bufferevent_get_output(upstream);

#ifdef TINYPROXY_DEBUG
	size_t len = evbuffer_get_length(src);

	LOG_DEBUG("stream pipe read",
		"line", _LOGV(conn->route->line_no),
		"src", _LOGV("client"),
		"dst", _LOGV("upstream"),
		"bytes", _LOGV(len)
	);
#endif

	if (conn->route->opts.sni_sniff) {
		stream_sniff_peek_client_input(&conn->sniff, src);
	}

	evbuffer_add_buffer(dst, src);

	if (evbuffer_get_length(dst) >= STREAM_READ_HIGH_WATER) {
		LOG_DEBUG("stream pipe backpressure pause",
			"line", _LOGV(conn->route->line_no),
			"paused", _LOGV("client"),
			"dst", _LOGV("upstream"),
			"dst_output_len", _LOGV(evbuffer_get_length(dst))
		);

		bufferevent_disable(client, EV_READ);
	}
}

void pipe_upstream_read_cb(struct bufferevent *upstream, void *arg)
{
	conn_t *conn = arg;
	struct bufferevent *client = conn->client;

	if (client == NULL) {
		return;
	}

	struct evbuffer *src = bufferevent_get_input(upstream);
	struct evbuffer *dst = bufferevent_get_output(client);

#ifdef TINYPROXY_DEBUG
	size_t len = evbuffer_get_length(src);

	LOG_DEBUG("stream pipe read",
		"line", _LOGV(conn->route->line_no),
		"src", _LOGV("upstream"),
		"dst", _LOGV("client"),
		"bytes", _LOGV(len)
	);
#endif

	evbuffer_add_buffer(dst, src);

	if (evbuffer_get_length(dst) >= STREAM_READ_HIGH_WATER) {
		LOG_DEBUG("stream pipe backpressure pause",
			"line", _LOGV(conn->route->line_no),
			"paused", _LOGV("upstream"),
			"dst", _LOGV("client"),
			"dst_output_len", _LOGV(evbuffer_get_length(dst))
		);

		bufferevent_disable(upstream, EV_READ);
	}
}

void pipe_client_write_cb(struct bufferevent *client, void *arg)
{
	conn_t *conn = arg;
	struct bufferevent *upstream = conn->upstream;

	struct evbuffer *output = bufferevent_get_output(client);
	size_t output_len = evbuffer_get_length(output);

	LOG_DEBUG("stream pipe write",
		"line", _LOGV(conn->route->line_no),
		"dst", _LOGV("client"),
		"src", _LOGV("upstream"),
		"dst_output_len", _LOGV(output_len)
	);

	if (conn->close_client_after_drain && output_len == 0) {
		LOG_DEBUG("client output drained; shutting down write side",
			"line", _LOGV(conn->route->line_no)
		);

		finish_client_write(conn);
		conn->close_client_after_drain = false;
		conn->close_after_client_eof = true;
		return;
	}

	if (upstream != NULL && output_len < STREAM_WRITE_RESUME_WATER) {
		LOG_DEBUG("stream pipe backpressure resume",
			"line", _LOGV(conn->route->line_no),
			"resumed", _LOGV("upstream"),
			"dst", _LOGV("client"),
			"dst_output_len", _LOGV(output_len)
		);

		bufferevent_enable(upstream, EV_READ);
	}
}

void pipe_upstream_write_cb(struct bufferevent *upstream, void *arg)
{
	conn_t *conn = arg;
	struct bufferevent *client = conn->client;

	struct evbuffer *output = bufferevent_get_output(upstream);
	size_t output_len = evbuffer_get_length(output);

	LOG_DEBUG("stream pipe write",
		"line", _LOGV(conn->route->line_no),
		"dst", _LOGV("upstream"),
		"src", _LOGV("client"),
		"dst_output_len", _LOGV(output_len)
	);

	if (client != NULL && output_len < STREAM_WRITE_RESUME_WATER) {
		LOG_DEBUG("stream pipe backpressure resume",
			"line", _LOGV(conn->route->line_no),
			"resumed", _LOGV("client"),
			"dst", _LOGV("upstream"),
			"dst_output_len", _LOGV(output_len)
		);

		bufferevent_enable(client, EV_READ);
	}
}
