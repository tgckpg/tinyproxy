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
		LOG_ERROR("unexpected argument: %s", argv[optind]);
		usage(stderr, argv[0]);
		return 2;
	}

	struct route *routes = NULL;
	size_t route_count = 0;

	int rc = load_routes_from_file(conf_path, &routes, &route_count);
	if (rc != 0) {
		LOG_ERROR("failed to load config %s: %s", conf_path, strerror(-rc));
		return 1;
	}

	if (route_count == 0) {
		LOG_ERROR("config %s has no routes", conf_path);
		free_routes(routes);
		return 1;
	}

	struct listener_ctx **tcp_ctxs = calloc(route_count, sizeof(*tcp_ctxs));
	if (tcp_ctxs == NULL) {
		free_routes(routes);
		return 1;
	}

	size_t tcp_ctx_count = 0;

	struct event_base *base = event_base_new();
	if (base == NULL) {
		LOG_ERROR("event_base_new failed");
		return 1;
	}

	struct signal_events signals;

	if (setup_signal_handlers(base, &signals) != 0) {
		LOG_ERROR("failed to setup signal handlers");
		event_base_free(base);
		free_routes(routes);
		return 1;
	}

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
			// rc = start_udp_route(r);
			LOG_WARN("skipping udp route: Not implemented yet");
			break;

		default:
			LOG_ERROR("route %zu has unknown protocol", i);
			rc = -EINVAL;
			break;
		}

		if (rc != 0) {
			LOG_ERROR("msg=\"failed to start route\" line=%u listen_host=%s listen_port=%u upstream_host=%s upstream_port=%u",
				r->line_no,
				r->listen_host, r->listen_port,
				r->upstream_host, r->upstream_port
			);

			for (size_t j = 0; j < tcp_ctx_count; j++) {
				free_tcp_route(tcp_ctxs[j]);
			}

			free(tcp_ctxs);
			event_base_free(base);
			free_routes(routes);
			return 1;
		}
	}

	rc = event_base_dispatch(base);

	for (size_t i = 0; i < tcp_ctx_count; i++) {
		free_tcp_route(tcp_ctxs[i]);
	}

	free_signal_handlers(&signals);
	free(tcp_ctxs);
	event_base_free(base);
	free_routes(routes);

	if (rc != 0) {
		LOG_ERROR("event loop failed: %s", strerror(-rc));
		return 1;
	}

	return 0;
}
