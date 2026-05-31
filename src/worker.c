#include <event2/event.h>
#include <event2/util.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "klog.h"
#include "stream_conn.h"
#include "datagram_client.h"
#include "datagram_listener.h"
#include "worker.h"

static void worker_notify_cb(evutil_socket_t fd, short events, void *arg);

static int notify_worker(struct worker *w)
{
	char byte = 1;
	int rc;

#ifdef _WIN32
	rc = send(w->notify_send_fd, &byte, 1, 0);
	if (rc == SOCKET_ERROR) {
		int err = WSAGetLastError();

		if (err == WSAEWOULDBLOCK) {
			return 0; /* already has pending wakeup */
		}

		return err;
	}
#else
	rc = (int)send(w->notify_send_fd, &byte, 1, 0);
	if (rc < 0) {
		int err = errno;

		if (err == EAGAIN || err == EWOULDBLOCK) {
			return 0; /* already has pending wakeup */
		}

		return err;
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

static struct worker_msg *worker_take_pending(struct worker *w)
{
	struct worker_msg *head;

	compat_mutex_lock(&w->pending_mu);

	head = w->pending_head;
	w->pending_head = NULL;
	w->pending_tail = NULL;

	compat_mutex_unlock(&w->pending_mu);

	return head;
}

static void worker_process_msg(struct worker *w, struct worker_msg *msg)
{
	switch (msg->kind) {
	case WORKER_MSG_STREAM_CLIENT:
		worker_adopt_client_fd(w, &msg->payload.stream_client);
		break;

	case WORKER_MSG_DATAGRAM_PACKET:
		cleanup_idle_datagram_clients(msg->payload.datagram_packet.ctx);
		datagram_route_handle_packet(w, &msg->payload.datagram_packet);
		break;

	default:
		LOG_WARN("unknown worker message kind",
			"worker", _LOGV(w->id),
			"kind", _LOGV(msg->kind)
		);
		break;
	}
}

static void free_processed_worker_msg(struct worker_msg *msg)
{
	if (!msg) {
		return;
	}

	switch (msg->kind) {
	case WORKER_MSG_STREAM_CLIENT:
		/*
		 * fd ownership moved to stream connection handling.
		 * Do not close it here.
		 */
		break;

	case WORKER_MSG_DATAGRAM_PACKET:
		free(msg->payload.datagram_packet.data);
		msg->payload.datagram_packet.data = NULL;
		msg->payload.datagram_packet.data_len = 0;
		break;
	}

	free(msg);
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

static void worker_process_pending(struct worker *w)
{
	struct worker_msg *msg = worker_take_pending(w);

	while (msg) {
		struct worker_msg *next = msg->next;

		worker_process_msg(w, msg);
		free_processed_worker_msg(msg);

		msg = next;
	}
}

static void free_worker_msg(struct worker_msg *msg)
{
	if (!msg) {
		return;
	}

	switch (msg->kind) {
	case WORKER_MSG_STREAM_CLIENT:
		if (socket_is_valid(msg->payload.stream_client.fd)) {
			evutil_closesocket(msg->payload.stream_client.fd);
			msg->payload.stream_client.fd = EVUTIL_INVALID_SOCKET;
		}
		break;
	case WORKER_MSG_DATAGRAM_PACKET:
		free(msg->payload.datagram_packet.data);
		msg->payload.datagram_packet.data = NULL;
		msg->payload.datagram_packet.data_len = 0;
		break;
	}

	free(msg);
}

void worker_free(struct worker *w)
{
	struct worker_msg *msg;

	if (!w) {
		return;
	}

	worker_stop(w);
	worker_join(w);

	msg = worker_take_pending(w);
	while (msg) {
		struct worker_msg *next = msg->next;

		free_worker_msg(msg);
		msg = next;
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

	klog_free();
}

static int worker_enqueue_msg(struct worker *w, struct worker_msg *msg)
{
	int rc;

	if (!w || !msg) {
		return EINVAL;
	}

	compat_mutex_lock(&w->pending_mu);

	if (w->stopping) {
		compat_mutex_unlock(&w->pending_mu);
		free_worker_msg(msg);
		return ECANCELED;
	}

	msg->next = NULL;

	if (w->pending_tail) {
		w->pending_tail->next = msg;
	} else {
		w->pending_head = msg;
	}

	w->pending_tail = msg;

	compat_mutex_unlock(&w->pending_mu);

	rc = notify_worker(w);
	if (rc != 0) {
		/*
		 * The message is already queued and owned by the worker.
		 * Do not free it here.
		 */
		LOG_WARN("failed to notify worker",
			"worker", _LOGV(w->id),
			"err", _LOGV(rc)
		);
	}

	return 0;
}

int worker_enqueue_stream_client(
	struct worker *w,
	const struct route *route,
	evutil_socket_t fd,
	const struct sockaddr *addr,
	socklen_t addr_len)
{
	struct worker_msg *msg;

	if (!w || !route || !socket_is_valid(fd)) {
		return EINVAL;
	}

	if (addr_len > sizeof(((struct worker_stream_client_msg *)0)->peer_addr)) {
		return EINVAL;
	}

	msg = calloc(1, sizeof(*msg));
	if (!msg) {
		return ENOMEM;
	}

	msg->kind = WORKER_MSG_STREAM_CLIENT;
	msg->payload.stream_client.route = route;
	msg->payload.stream_client.fd = fd;

	if (addr && addr_len > 0) {
		memcpy(&msg->payload.stream_client.peer_addr, addr, addr_len);
		msg->payload.stream_client.peer_addr_len = addr_len;
	}

	return worker_enqueue_msg(w, msg);
}

int worker_enqueue_datagram_packet(
	struct worker *w,
	struct datagram_route_ctx *ctx,
	const struct sockaddr *peer_addr,
	socklen_t peer_addr_len,
	const unsigned char *data,
	size_t data_len)
{
	struct worker_msg *msg;
	const struct route *route = ctx->route;

	if (!w || !route || !socket_is_valid(ctx->listen_fd)) {
		return EINVAL;
	}

	if (!peer_addr || peer_addr_len == 0 ||
	    peer_addr_len > sizeof(((struct worker_datagram_packet_msg *)0)->peer_addr)) {
		return EINVAL;
	}

	if (!data && data_len > 0) {
		return EINVAL;
	}

	msg = calloc(1, sizeof(*msg));
	if (!msg) {
		return ENOMEM;
	}

	msg->kind = WORKER_MSG_DATAGRAM_PACKET;
	msg->payload.datagram_packet.route = route;
	msg->payload.datagram_packet.ctx = ctx;

	memcpy(
		&msg->payload.datagram_packet.peer_addr,
		peer_addr,
		peer_addr_len
	);
	msg->payload.datagram_packet.peer_addr_len = peer_addr_len;

	if (data_len > 0) {
		msg->payload.datagram_packet.data = malloc(data_len);
		if (!msg->payload.datagram_packet.data) {
			free(msg);
			return ENOMEM;
		}

		memcpy(msg->payload.datagram_packet.data, data, data_len);
		msg->payload.datagram_packet.data_len = data_len;
	}

	return worker_enqueue_msg(w, msg);
}

static void worker_notify_cb(evutil_socket_t fd, short events, void *arg)
{
	struct worker *w = arg;
	bool stopping;

	(void)events;

	drain_notify_fd(fd);

	worker_process_pending(w);

	compat_mutex_lock(&w->pending_mu);
	stopping = w->stopping;
	compat_mutex_unlock(&w->pending_mu);

	if (stopping) {
		event_base_loopbreak(w->base);
	}
}
