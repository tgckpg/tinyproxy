#include <event2/buffer.h>

#include <stdlib.h>
#include <string.h>

#include "klog.h"
#include "route.h"
#include "stream_file.h"
#include "stream_conn.h"

#define FILE_CHUNK_SIZE 4096

struct file_conn {
	conn_t *conn;
	FILE *fp;
};

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

	while (evbuffer_get_length(out) < STREAM_READ_HIGH_WATER) {
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

int start_stream_file(conn_t *conn)
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

	bufferevent_setwatermark(conn->client, EV_WRITE, 0, STREAM_READ_HIGH_WATER);
	bufferevent_setcb(conn->client, NULL, file_write_cb, file_event_cb, fc);
	bufferevent_enable(conn->client, EV_WRITE);

	set_client_idle_timeout(conn, r);

	file_write_cb(conn->client, fc);

	return 0;
}
