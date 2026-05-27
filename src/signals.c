#include "signals.h"

#include <signal.h>
#include <string.h>

#include "klog.h"

static void signal_cb(evutil_socket_t sig, short events, void *arg)
{
	(void)events;

	struct event_base *base = arg;

	switch (sig) {
	case SIGINT:
		LOG_INFO("received signal, shutting down", "signal", _LOGV("SIGINT"));
		break;
	case SIGTERM:
		LOG_INFO("received signal, shutting down", "signal", _LOGV("SIGTERM"));
		break;
	default:
		LOG_INFO("received signal, shutting down", "signal", _LOGV("unknown"));
		break;
	}

	event_base_loopbreak(base);
}

int setup_signal_handlers(struct event_base *base, struct signal_events *signals)
{
	if (base == NULL || signals == NULL) {
		return -1;
	}

	memset(signals, 0, sizeof(*signals));

	signals->sigint_ev = evsignal_new(base, SIGINT, signal_cb, base);
	if (signals->sigint_ev == NULL) {
		free_signal_handlers(signals);
		return -1;
	}

	signals->sigterm_ev = evsignal_new(base, SIGTERM, signal_cb, base);
	if (signals->sigterm_ev == NULL) {
		free_signal_handlers(signals);
		return -1;
	}

	if (event_add(signals->sigint_ev, NULL) != 0) {
		free_signal_handlers(signals);
		return -1;
	}

	if (event_add(signals->sigterm_ev, NULL) != 0) {
		free_signal_handlers(signals);
		return -1;
	}

	return 0;
}

void free_signal_handlers(struct signal_events *signals)
{
	if (signals == NULL) {
		return;
	}

	event_free(signals->sigint_ev);
	event_free(signals->sigterm_ev);

	signals->sigint_ev = NULL;
	signals->sigterm_ev = NULL;
}
