#ifndef WORKER_H
#define WORKER_H

struct event_base;

struct worker {
	struct event_base *base;
	size_t id;
};

#endif
