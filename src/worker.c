#include <event2/event.h>
#include <event2/util.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "klog.h"
#include "stream_conn.h"
#include "worker.h"

static void worker_notify_cb(evutil_socket_t fd, short events, void *arg);

static int notify_worker(struct worker *w)
{
	char byte = 1;
	int rc;

	(void)w;

#ifdef _WIN32
	rc = send(w->notify_send_fd, &byte, 1, 0);
	if (rc == SOCKET_ERROR) {
		return WSAGetLastError();
	}
#else
	rc = (int)send(w->notify_send_fd, &byte, 1, 0);
	if (rc < 0) {
		return errno;
	}
#endif

	return 0;
}

static void drain_notify_fd(evutil_socket_t fd)
{
	char buf[128];

	for (;;) {
#ifdef _WIN32
		int n = recv(fd, buf, sizeof(buf), 0);
		if (n == SOCKET_ERROR) {
			int err = WSAGetLastError();
			if (err == WSAEWOULDBLOCK) {
				return;
			}
			return;
		}
#else
		ssize_t n = recv(fd, buf, sizeof(buf), 0);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return;
			}
			return;
		}
#endif

		if (n == 0) {
			return;
		}

		if ((size_t)n < sizeof(buf)) {
			return;
		}
	}
}

static struct accepted_client_node *worker_take_pending(struct worker *w)
{
	struct accepted_client_node *head;

	compat_mutex_lock(&w->pending_mu);

	head = w->pending_head;
	w->pending_head = NULL;
	w->pending_tail = NULL;

	compat_mutex_unlock(&w->pending_mu);

	return head;
}

static void worker_process_pending(struct worker *w)
{
	struct accepted_client_node *node = worker_take_pending(w);

	while (node) {
		struct accepted_client_node *next = node->next;

		worker_adopt_client_fd(w, &node->client);

		free(node);
		node = next;
	}
}

static void *worker_main(void *arg)
{
	struct worker *w = arg;
	klog_set_worker_id(w->id);

	event_base_dispatch(w->base);

	return NULL;
}

int worker_init(struct worker *w, unsigned int id)
{
	evutil_socket_t fds[2] = {
		EVUTIL_INVALID_SOCKET,
		EVUTIL_INVALID_SOCKET,
	};

	if (!w) {
		return EINVAL;
	}

	memset(w, 0, sizeof(*w));

	w->id = id;
	w->notify_recv_fd = EVUTIL_INVALID_SOCKET;
	w->notify_send_fd = EVUTIL_INVALID_SOCKET;

	w->base = event_base_new();
	if (!w->base) {
		return ENOMEM;
	}

	if (compat_mutex_init(&w->pending_mu) != 0) {
		event_base_free(w->base);
		w->base = NULL;
		return errno ? errno : EINVAL;
	}

	if (evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
		compat_mutex_destroy(&w->pending_mu);
		event_base_free(w->base);
		w->base = NULL;
		return EVUTIL_SOCKET_ERROR();
	}

	w->notify_recv_fd = fds[0];
	w->notify_send_fd = fds[1];

	evutil_make_socket_nonblocking(w->notify_recv_fd);
	evutil_make_socket_nonblocking(w->notify_send_fd);
	evutil_make_socket_closeonexec(w->notify_recv_fd);
	evutil_make_socket_closeonexec(w->notify_send_fd);

	w->notify_event = event_new(
		w->base,
		w->notify_recv_fd,
		EV_READ | EV_PERSIST,
		worker_notify_cb,
		w
	);
	if (!w->notify_event) {
		evutil_closesocket(w->notify_recv_fd);
		evutil_closesocket(w->notify_send_fd);
		w->notify_recv_fd = EVUTIL_INVALID_SOCKET;
		w->notify_send_fd = EVUTIL_INVALID_SOCKET;

		compat_mutex_destroy(&w->pending_mu);
		event_base_free(w->base);
		w->base = NULL;

		return ENOMEM;
	}

	if (event_add(w->notify_event, NULL) < 0) {
		event_free(w->notify_event);
		w->notify_event = NULL;

		evutil_closesocket(w->notify_recv_fd);
		evutil_closesocket(w->notify_send_fd);
		w->notify_recv_fd = EVUTIL_INVALID_SOCKET;
		w->notify_send_fd = EVUTIL_INVALID_SOCKET;

		compat_mutex_destroy(&w->pending_mu);
		event_base_free(w->base);
		w->base = NULL;

		return EINVAL;
	}

	return 0;
}

int worker_start(struct worker *w)
{
	int rc;

	if (!w || !w->base) {
		return EINVAL;
	}

	if (w->started) {
		return 0;
	}

	rc = compat_thread_create(&w->thread, worker_main, w);
	if (rc != 0) {
		return rc;
	}

	w->started = true;

	return 0;
}

void worker_stop(struct worker *w)
{
	if (!w) {
		return;
	}

	compat_mutex_lock(&w->pending_mu);
	w->stopping = true;
	compat_mutex_unlock(&w->pending_mu);

	if (w->started) {
		(void)notify_worker(w);
	}
}

void worker_join(struct worker *w)
{
	if (!w || !w->started) {
		return;
	}

	(void)compat_thread_join(w->thread);
	w->started = false;
}

static int socket_is_valid(evutil_socket_t fd)
{
    return fd != (evutil_socket_t)EVUTIL_INVALID_SOCKET;
}

void worker_free(struct worker *w)
{
	struct accepted_client_node *node;

	if (!w) {
		return;
	}

	worker_stop(w);
	worker_join(w);

	node = worker_take_pending(w);
	while (node) {
		struct accepted_client_node *next = node->next;

		if (socket_is_valid(node->client.fd)) {
			evutil_closesocket(node->client.fd);
		}

		free(node);
		node = next;
	}

	if (w->notify_event) {
		event_free(w->notify_event);
		w->notify_event = NULL;
	}

	if (socket_is_valid(w->notify_recv_fd)) {
		evutil_closesocket(w->notify_recv_fd);
		w->notify_recv_fd = EVUTIL_INVALID_SOCKET;
	}

	if (socket_is_valid(w->notify_send_fd)) {
		evutil_closesocket(w->notify_send_fd);
		w->notify_send_fd = EVUTIL_INVALID_SOCKET;
	}

	compat_mutex_destroy(&w->pending_mu);

	if (w->base) {
		event_base_free(w->base);
		w->base = NULL;
	}
}

int worker_enqueue_client_fd(
	struct worker *w,
	const struct route *route,
	evutil_socket_t fd,
	const struct sockaddr *addr,
	socklen_t addr_len
) {
	struct accepted_client_node *node;
	int rc;

	if (!w || !route || !socket_is_valid(fd)) {
		return EINVAL;
	}

	if (addr_len > sizeof(node->client.peer_addr)) {
		return EINVAL;
	}

	node = calloc(1, sizeof(*node));
	if (!node) {
		return ENOMEM;
	}

	node->client.route = route;
	node->client.fd = fd;

	if (addr && addr_len > 0) {
		memcpy(&node->client.peer_addr, addr, addr_len);
		node->client.peer_addr_len = addr_len;
	}

	compat_mutex_lock(&w->pending_mu);

	if (w->stopping) {
		compat_mutex_unlock(&w->pending_mu);
		free(node);
		return ECANCELED;
	}

	if (w->pending_tail) {
		w->pending_tail->next = node;
	} else {
		w->pending_head = node;
	}

	w->pending_tail = node;

	compat_mutex_unlock(&w->pending_mu);

	rc = notify_worker(w);
	if (rc != 0) {
		/*
		 * The fd is already queued and owned by the worker.
		 * Do not close it here.
		 */
		LOG_WARN("failed to notify worker",
			"worker", _LOGV(w->id),
			"err", _LOGV(rc)
		);
	}

	return 0;
}

static void worker_notify_cb(evutil_socket_t fd, short events, void *arg)
{
	struct worker *w = arg;

	(void)events;

	drain_notify_fd(fd);

	worker_process_pending(w);

	compat_mutex_lock(&w->pending_mu);
	bool stopping = w->stopping;
	compat_mutex_unlock(&w->pending_mu);

	if (stopping) {
		event_base_loopbreak(w->base);
	}
}
