#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <event2/event.h>

#include "route.h"
#include "tcp_route.h"
#include "klog.h"

static void set_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) {
		(void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size == 0 || size > 8192) {
		return 0;
	}

	struct event_base *base = event_base_new();
	if (base == NULL) {
		return 0;
	}

	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		event_base_free(base);
		return 0;
	}

	set_nonblock(sv[0]);
	set_nonblock(sv[1]);

	struct route r;
	memset(&r, 0, sizeof(r));

	r.proto = PROTO_TCP;
	r.line_no = 1;

	/*
	 * listen endpoint is not used by the fuzz seam, but keep it sane.
	 */
	r.listen.kind = ENDPOINT_INET;
	snprintf(r.listen.host, sizeof(r.listen.host), "127.0.0.1");
	r.listen.port = 12345;

	/*
	 * Important: this should be something connect_upstream() can handle.
	 * For a first fuzzer, intentionally point it at a closed port.
	 * That fuzzes failure paths without needing an upstream server.
	 */
	r.upstream.kind = ENDPOINT_INET;
	snprintf(r.upstream.host, sizeof(r.upstream.host), "127.0.0.1");
	r.upstream.port = 9;

	r.opts.proxy_v2 = (data[0] & 1) != 0;
	r.opts.keep_alive = (data[0] & 2) != 0;
	r.opts.idle_timeout_sec = 1;
	r.opts.connect_timeout_sec = 1;

	struct sockaddr_storage peer;
	memset(&peer, 0, sizeof(peer));

	struct sockaddr_un *sun = (struct sockaddr_un *)&peer;
	sun->sun_family = AF_UNIX;

	if (tcp_route_adopt_client_for_fuzz(
		    base,
		    &r,
		    sv[1],
		    &peer,
		    sizeof(*sun)) != 0) {
		close(sv[0]);
		close(sv[1]);
		event_base_free(base);
		return 0;
	}

	/*
	 * Ownership of sv[1] was transferred to bufferevent with
	 * BEV_OPT_CLOSE_ON_FREE, so don't close sv[1] manually after success.
	 */
	sv[1] = -1;

	(void)write(sv[0], data, size);

	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 1000;

	event_base_loopexit(base, &tv);
	event_base_dispatch(base);

	close(sv[0]);
	event_base_free(base);

	return 0;
}
