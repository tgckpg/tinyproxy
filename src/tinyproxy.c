#include <event2/event.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "klog.h"
#include "signal.h"
#include "file_conf.h"
#include "tcp_route.h"
#include "udp_route.h"

static void usage(FILE *out, const char *prog)
{
	fprintf(out,
		"usage: %s [-c config-file]\n"
		"\n"
		"options:\n"
		"  -c FILE   path to config file (default: tinyproxy.conf)\n"
		"  -v		show the version\n"
		"  -h		show this help\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *conf_path = "tinyproxy.conf";
	int opt;
	int exit_code = 1;
	int rc;

#ifdef _WIN32
	int wsa_started = 0;
#endif

	struct route *routes = NULL;
	size_t route_count = 0;

	struct tcp_route_ctx **tcp_ctxs = NULL;
	struct udp_route_ctx **udp_ctxs = NULL;
	size_t tcp_ctx_count = 0;
	size_t udp_ctx_count = 0;

	struct event_base *base = NULL;
	struct signal_events signals;
	int signals_started = 0;

	while ((opt = getopt(argc, argv, "c:h:v")) != -1) {
		switch (opt) {
		case 'c':
			if (optarg == NULL || optarg[0] == '\0') {
				LOG_ERROR("missing config path after -c");
				return 2;
			}
			conf_path = optarg;
			break;

		case 'h':
			usage(stdout, argv[0]);
			return 0;

		case 'v':
			fprintf(stdout, "tinyproxy v0.0.1\n");
			return 0;

		default:
			usage(stderr, argv[0]);
			return 2;
		}
	}

	if (optind != argc) {
		LOG_ERROR("unexpected argument", "argv", _LOGV(argv[optind]));
		usage(stderr, argv[0]);
		return 2;
	}

	rc = load_routes_from_file(conf_path, &routes, &route_count);
	if (rc != 0) {
		LOG_ERROR("failed to load config",
			"path", _LOGV(conf_path),
			"err", _LOGV(strerror(-rc)));
		goto out;
	}

	if (route_count == 0) {
		LOG_ERROR("config has no routes", "path", _LOGV(conf_path));
		goto out;
	}

	tcp_ctxs = calloc(route_count, sizeof(*tcp_ctxs));
	if (tcp_ctxs == NULL) {
		LOG_ERROR("tcp calloc failed", "err", _LOGV(strerror(errno)));
		goto out;
	}

	udp_ctxs = calloc(route_count, sizeof(*udp_ctxs));
	if (udp_ctxs == NULL) {
		LOG_ERROR("udp calloc failed", "err", _LOGV(strerror(errno)));
		goto out;
	}

#ifdef _WIN32
	WSADATA wsa;
	rc = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (rc != 0) {
		LOG_ERROR("WSAStartup failed", "err", _LOGV(rc));
		goto out;
	}
	wsa_started = 1;
#endif

	base = event_base_new();
	if (base == NULL) {
		LOG_ERROR("event_base_new failed");
		goto out;
	}

	if (setup_signal_handlers(base, &signals) != 0) {
		LOG_ERROR("failed to setup signal handlers");
		goto out;
	}
	signals_started = 1;

	struct worker w = {
		.base = base,
		.id = 0,
	};

	for (size_t i = 0; i < route_count; i++) {
		const struct route *r = &routes[i];

		switch (r->proto) {
		case PROTO_TCP:
			rc = start_tcp_route(&w, r, &tcp_ctxs[tcp_ctx_count]);
			if (rc == 0) {
				tcp_ctx_count++;
			}
			break;

		case PROTO_UDP:
			rc = start_udp_route(&w, r, &udp_ctxs[udp_ctx_count]);
			if (rc == 0) {
				udp_ctx_count++;
			}
			break;

		default:
			LOG_ERROR("route has unknown protocol", "line", _LOGV(r->line_no));
			rc = -EINVAL;
			break;
		}

		if (rc != 0) {
			LOG_ERROR("failed to start route",
				"line", _LOGV(r->line_no),
				"listen", _LOGV_ENDPOINT(&r->listen),
				"upstream", _LOGV_ENDPOINT(&r->upstream)
			);
			goto out;
		}
	}

	rc = event_base_dispatch(base);
	if (rc != 0) {
		LOG_ERROR("event loop failed", "err", _LOGV(rc));
		goto out;
	}

	exit_code = 0;

out:
	for (size_t i = 0; i < tcp_ctx_count; i++) {
		free_tcp_route(tcp_ctxs[i]);
	}

	if (signals_started) {
		free_signal_handlers(&signals);
	}

	free(tcp_ctxs);

	if (base != NULL) {
		event_base_free(base);
	}

	free_routes(routes);

#ifdef _WIN32
	if (wsa_started) {
		WSACleanup();
	}
#endif

	return exit_code;
}
