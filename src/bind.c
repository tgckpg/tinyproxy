#include "klog.h"
#include "compat_cpu.h"

#include "bind.h"

#define BIND_WAIT_INTERVAL_MS 100

int bind_with_wait(
	evutil_socket_t fd,
	const struct sockaddr *addr,
	socklen_t addr_len,
	const struct route *r)
{
	int waited_ms = 0;
	int wait_ms = 1000 * r->opts.bind_wait_sec;

	for (;;) {
		if (bind(fd, addr, addr_len) == 0) {
			if(0 < waited_ms) {
				LOG_INFO("listener bound after wait",
					"listen", _LOGV_ENDPOINT(&r->listen));
			}
			return 0;
		}

		int err = EVUTIL_SOCKET_ERROR();

		if (err != EADDRINUSE || wait_ms <= 0 || waited_ms >= wait_ms) {
			return err ? -err : -EIO;
		}

		if (waited_ms == 0) {
			LOG_WARN("listener address in use, waiting to bind",
				"listen", _LOGV_ENDPOINT(&r->listen),
				"timeout", _LOGV(r->opts.bind_wait_sec));
		}

		sleep_ms(BIND_WAIT_INTERVAL_MS);
		waited_ms += BIND_WAIT_INTERVAL_MS;
	}
}

