#ifndef STREAM_CONN_H
#define STREAM_CONN_H

#include "stream_route.h"

void dispatch_client_fd(struct worker *w, struct accepted_client *ac);
void free_conn(conn_t *conn);

void set_client_idle_timeout(conn_t *conn, const struct route *r);
void event_cb(struct bufferevent *bev, short events, void *arg);

#endif
