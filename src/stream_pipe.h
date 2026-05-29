#ifndef STREAM_PIPE_H
#define STREAM_PIPE_H

#include <event2/bufferevent.h>

void pipe_client_read_cb(struct bufferevent *client, void *arg);
void pipe_client_write_cb(struct bufferevent *client, void *arg);
void pipe_upstream_read_cb(struct bufferevent *upstream, void *arg);
void pipe_upstream_write_cb(struct bufferevent *upstream, void *arg);

#endif
