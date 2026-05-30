#ifndef WORKER_H
#define WORKER_H

#include <stdbool.h>
#include <stddef.h>

#include <event2/event.h>
#include <event2/util.h>

#include "compat_thread.h"
#include "compat.h"
#include "route.h"

struct accept_client_node;

struct worker {
	unsigned int id;

	struct event_base *base;

	compat_thread_t thread;
	bool started;
	bool stopping;

	evutil_socket_t notify_recv_fd;
	evutil_socket_t notify_send_fd;
	struct event *notify_event;

	struct worker_msg *pending_head;
	struct worker_msg *pending_tail;
	compat_mutex_t pending_mu;
};

enum worker_msg_kind {
	WORKER_MSG_STREAM_CLIENT,
};

struct worker_stream_client_msg {
	const struct route *route;
	evutil_socket_t fd;
	struct sockaddr_storage peer_addr;
	socklen_t peer_addr_len;
};

struct worker_msg {
	enum worker_msg_kind kind;
	struct worker_msg *next;

	union {
		struct worker_stream_client_msg stream_client;
	} payload;
};

int worker_init(struct worker *w, unsigned int id);
int worker_start(struct worker *w);
void worker_stop(struct worker *w);
void worker_join(struct worker *w);
void worker_free(struct worker *w);

int worker_enqueue_stream_client(
	struct worker *w,
	const struct route *route,
	evutil_socket_t fd,
	const struct sockaddr *addr,
	socklen_t addr_len
);

#endif
