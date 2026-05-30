#include "compat_thread.h"

#ifdef _WIN32

#include <errno.h>
#include <stdlib.h>

struct compat_thread_start {
	compat_thread_fn fn;
	void *arg;
};

static DWORD WINAPI compat_thread_trampoline(LPVOID raw)
{
	struct compat_thread_start *start = raw;
	compat_thread_fn fn = start->fn;
	void *arg = start->arg;

	free(start);

	fn(arg);

	return 0;
}

int compat_thread_create(
	compat_thread_t *thread,
	compat_thread_fn fn,
	void *arg
) {
	struct compat_thread_start *start;

	if (!thread || !fn) {
		return EINVAL;
	}

	start = malloc(sizeof(*start));
	if (!start) {
		return ENOMEM;
	}

	start->fn = fn;
	start->arg = arg;

	*thread = CreateThread(
		NULL,
		0,
		compat_thread_trampoline,
		start,
		0,
		NULL
	);

	if (!*thread) {
		free(start);
		return (int)GetLastError();
	}

	return 0;
}

int compat_thread_join(compat_thread_t thread)
{
	DWORD rc;

	if (!thread) {
		return EINVAL;
	}

	rc = WaitForSingleObject(thread, INFINITE);
	if (rc != WAIT_OBJECT_0) {
		return (int)GetLastError();
	}

	CloseHandle(thread);

	return 0;
}

int compat_mutex_init(compat_mutex_t *mutex)
{
	if (!mutex) {
		return EINVAL;
	}

	InitializeCriticalSection(mutex);

	return 0;
}

void compat_mutex_lock(compat_mutex_t *mutex)
{
	EnterCriticalSection(mutex);
}

void compat_mutex_unlock(compat_mutex_t *mutex)
{
	LeaveCriticalSection(mutex);
}

void compat_mutex_destroy(compat_mutex_t *mutex)
{
	DeleteCriticalSection(mutex);
}

#else

#include <errno.h>

int compat_thread_create(
	compat_thread_t *thread,
	compat_thread_fn fn,
	void *arg
) {
	if (!thread || !fn) {
		return EINVAL;
	}

	return pthread_create(thread, NULL, fn, arg);
}

int compat_thread_join(compat_thread_t thread)
{
	return pthread_join(thread, NULL);
}

int compat_mutex_init(compat_mutex_t *mutex)
{
	if (!mutex) {
		return EINVAL;
	}

	return pthread_mutex_init(mutex, NULL);
}

void compat_mutex_lock(compat_mutex_t *mutex)
{
	pthread_mutex_lock(mutex);
}

void compat_mutex_unlock(compat_mutex_t *mutex)
{
	pthread_mutex_unlock(mutex);
}

void compat_mutex_destroy(compat_mutex_t *mutex)
{
	pthread_mutex_destroy(mutex);
}

#endif
