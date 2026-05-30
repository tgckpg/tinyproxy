#include "worker_pool.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int worker_pool_init(struct worker_pool *pool, size_t count)
{
	size_t i;

	if (!pool || count == 0) {
		return EINVAL;
	}

	memset(pool, 0, sizeof(*pool));

	pool->workers = calloc(count, sizeof(pool->workers[0]));
	if (!pool->workers) {
		return ENOMEM;
	}

	pool->count = count;

	for (i = 0; i < count; i++) {
		int rc = worker_init(&pool->workers[i], (unsigned int)i);
		if (rc != 0) {
			while (i > 0) {
				i--;
				worker_free(&pool->workers[i]);
			}

			free(pool->workers);
			pool->workers = NULL;
			pool->count = 0;

			return rc;
		}
	}

	return 0;
}

int worker_pool_start(struct worker_pool *pool)
{
	size_t i;

	if (!pool || !pool->workers) {
		return EINVAL;
	}

	for (i = 0; i < pool->count; i++) {
		int rc = worker_start(&pool->workers[i]);
		if (rc != 0) {
			while (i > 0) {
				i--;
				worker_stop(&pool->workers[i]);
				worker_join(&pool->workers[i]);
			}

			return rc;
		}
	}

	return 0;
}

void worker_pool_stop(struct worker_pool *pool)
{
	size_t i;

	if (!pool || !pool->workers) {
		return;
	}

	for (i = 0; i < pool->count; i++) {
		worker_stop(&pool->workers[i]);
	}
}

void worker_pool_join(struct worker_pool *pool)
{
	size_t i;

	if (!pool || !pool->workers) {
		return;
	}

	for (i = 0; i < pool->count; i++) {
		worker_join(&pool->workers[i]);
	}
}

void worker_pool_free(struct worker_pool *pool)
{
	size_t i;

	if (!pool || !pool->workers) {
		return;
	}

	for (i = 0; i < pool->count; i++) {
		worker_free(&pool->workers[i]);
	}

	free(pool->workers);
	memset(pool, 0, sizeof(*pool));
}

struct worker *worker_pool_next(struct worker_pool *pool)
{
	struct worker *w;

	if (!pool || !pool->workers || pool->count == 0) {
		return NULL;
	}

	w = &pool->workers[pool->next % pool->count];
	pool->next++;

	return w;
}