#ifndef STREAM_CONN_H
#define STREAM_CONN_H

#include "stream_route.h"

struct worker_stream_client_msg;

int dispatch_client_fd(struct worker *w,
	const struct route *route,
	evutil_socket_t fd,
	const struct sockaddr *addr,
	socklen_t addr_len);
void free_conn(conn_t *conn);

void finish_client_write(conn_t *conn);
void set_client_idle_timeout(conn_t *conn, const struct route *r);

void stream_client_event_cb(struct bufferevent *bev, short events, void *arg);
void stream_upstream_event_cb(struct bufferevent *bev, short events, void *arg);

void worker_adopt_client_fd(struct worker *w, struct worker_stream_client_msg *ac);

static inline void conn_touch(conn_t *conn)
{
    conn->activity_seq++;
}

#ifdef FUZZ
int stream_route_adopt_client_for_fuzz(
	struct event_base *base,
	const struct route *r,
	evutil_socket_t client_fd,
	const struct sockaddr_storage *peer_addr,
	socklen_t peer_addr_len);
#endif

#endif
