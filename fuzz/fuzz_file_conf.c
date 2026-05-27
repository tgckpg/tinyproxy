#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "file_conf.h"

static void check_endpoint(const struct endpoint *ep)
{
	switch (ep->kind) {
	case ENDPOINT_INET:
		if (ep->port == 0) {
			abort();
		}
		if (ep->host[0] == '\0') {
			abort();
		}
		break;

	case ENDPOINT_UNIX:
	case ENDPOINT_UNIX_DGRAM:
		if (ep->path[0] == '\0') {
			abort();
		}
		break;

	default:
		abort();
	}
}

static void check_route(const struct route *r)
{
	check_endpoint(&r->listen);
	check_endpoint(&r->upstream);

	switch (r->proto) {
	case PROTO_TCP:
	case PROTO_UDP:
		break;
	default:
		abort();
	}

	if (r->line_no == 0) {
		abort();
	}

	if (r->opts.idle_timeout_sec <= 0) {
		abort();
	}

	if (r->opts.connect_timeout_sec <= 0) {
		abort();
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	/*
	 * Keep the fuzzer fast. file_conf.c has MAX_LINE_LEN=512, so this is
	 * already enough to exercise normal, multiline, and overlong cases.
	 */
	if (size > 8192) {
		return 0;
	}

	char path[] = "/tmp/tinyproxy-file-conf-fuzz-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) {
		return 0;
	}

	size_t off = 0;
	while (off < size) {
		ssize_t n = write(fd, data + off, size - off);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}

			close(fd);
			unlink(path);
			return 0;
		}

		if (n == 0) {
			break;
		}

		off += (size_t)n;
	}

	close(fd);

	struct route *routes = NULL;
	size_t count = 0;

	int rc = load_routes_from_file(path, &routes, &count);

	unlink(path);

	if (rc == 0) {
		if (count > 0 && routes == NULL) {
			abort();
		}

		for (size_t i = 0; i < count; i++) {
			check_route(&routes[i]);
		}

		free_routes(routes);
	} else {
		/*
		 * On failure, load_routes_from_file() should not transfer ownership.
		 * Keep this loose because callers should not rely on output values
		 * after error unless the API explicitly promises that.
		 */
	}

	return 0;
}
