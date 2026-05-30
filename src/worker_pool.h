#ifndef WORKER_POOL_H
#define WORKER_POOL_H

#include <stddef.h>

#include "worker.h"

struct worker_pool {
	struct worker *workers;
	size_t count;
	size_t next;
};

int worker_pool_init(struct worker_pool *pool, size_t count);
int worker_pool_start(struct worker_pool *pool);
void worker_pool_stop(struct worker_pool *pool);
void worker_pool_join(struct worker_pool *pool);
void worker_pool_free(struct worker_pool *pool);

struct worker *worker_pool_next(struct worker_pool *pool);

#endif
