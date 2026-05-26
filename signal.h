#ifndef SIGNAL_H
#define SIGNAL_H

#include <event2/event.h>

struct signal_events {
	struct event *sigint_ev;
	struct event *sigterm_ev;
};

int setup_signal_handlers(struct event_base *base, struct signal_events *signals);
void free_signal_handlers(struct signal_events *signals);

#endif
