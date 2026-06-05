#include <event2/buffer.h>

#include <string.h>

#include "klog.h"
#include "stream_builtin.h"
#include "stream_conn.h"

static void builtin_client_read_cb(struct bufferevent *bev, void *arg)
{
	struct evbuffer *input = bufferevent_get_input(bev);

	(void)arg;

	evbuffer_drain(input, evbuffer_get_length(input));
}

static void builtin_close_after_write_cb(struct bufferevent *bev, void *arg)
{
	conn_t *conn = arg;
	struct evbuffer *output = bufferevent_get_output(bev);

	if (evbuffer_get_length(output) != 0) {
		return;
	}

	finish_client_write(conn);
	conn->close_after_client_eof = true;
}

static const char *stream_client_addr_string(conn_t *conn, char *buf, size_t buf_len)
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

int start_stream_builtin(conn_t *conn)
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
	req.client_addr = stream_client_addr_string(conn, client_addr, sizeof(client_addr));

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
			bufferevent_setcb(conn->client, NULL, builtin_close_after_write_cb, stream_client_event_cb, conn);
			bufferevent_enable(conn->client, EV_WRITE);
		} else {
			free_conn(conn);
		}
		return 0;

	case X_BUILTIN_ACTION_DISCARD:
		bufferevent_setcb(conn->client, builtin_client_read_cb, NULL, stream_client_event_cb, conn);
		bufferevent_enable(conn->client, EV_READ);
		return 0;

	case X_BUILTIN_ACTION_HANG:
		bufferevent_setcb(conn->client, NULL, NULL, stream_client_event_cb, conn);
		bufferevent_enable(conn->client, EV_READ);
		return 0;

	default:
		free_conn(conn);
		return -EINVAL;
	}
}
