#ifndef TINYPROXY_COMPAT_THREAD_H
#define TINYPROXY_COMPAT_THREAD_H

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*compat_thread_fn)(void *arg);

#ifdef _WIN32
typedef HANDLE compat_thread_t;
typedef CRITICAL_SECTION compat_mutex_t;
#else
typedef pthread_t compat_thread_t;
typedef pthread_mutex_t compat_mutex_t;
#endif

int compat_thread_create(
	compat_thread_t *thread,
	compat_thread_fn fn,
	void *arg
);

int compat_thread_join(compat_thread_t thread);

int compat_mutex_init(compat_mutex_t *mutex);
void compat_mutex_lock(compat_mutex_t *mutex);
void compat_mutex_unlock(compat_mutex_t *mutex);
void compat_mutex_destroy(compat_mutex_t *mutex);

#ifdef __cplusplus
}
#endif

#endif
