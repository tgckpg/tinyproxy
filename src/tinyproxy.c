#include <event2/event.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "klog.h"
#include "signals.h"
#include "file_conf.h"
#include "worker_pool.h"
#include "route.h"
#include "route_runtime.h"

static void usage(FILE *out, const char *prog)
{
	fprintf(out,
		"usage: %s [-c config-file | -L route]\n"
		"\n"
		"options:\n"
		"  -c FILE   path to config file (default: tinyproxy.conf)\n"
		"  -L ROUTE  add inline route, same syntax as config line\n"
		"            may be specified multiple times\n"
		"  -v        show the version\n"
		"  -h        show this help\n",
		prog);
}

static void prevent_socket_write_from_killing_process(void)
{
#ifndef _WIN32
	/*
	 * On Unix, writing to a closed socket may raise SIGPIPE.
	 * The default SIGPIPE action is process termination.
	 *
	 * Proxies must treat closed peers as ordinary I/O errors,
	 * not as a reason to kill the whole daemon.
	 */
	signal(SIGPIPE, SIG_IGN);
#endif
}

int main(int argc, char **argv)
{
	prevent_socket_write_from_killing_process();

	int opt;
	int exit_code = 1;
	int rc;

	const char **inline_routes = NULL;
	size_t inline_route_count = 0;
	size_t inline_route_cap = 0;
	bool worker_pool_ready = false;
	bool worker_pool_started = false;

#ifdef _WIN32
	int wsa_started = 0;
#endif

	const char *conf_path = NULL;
	struct route *routes = NULL;
	size_t route_count = 0;

	struct route_ctx *route_ctxs = NULL;
	size_t route_ctx_count = 0;

	struct event_base *base = NULL;
	struct signal_events signals;
	int signals_started = 0;

	while ((opt = getopt(argc, argv, "c:L:h:v")) != -1) {
		switch (opt) {
			case 'c':
				if (optarg == NULL || optarg[0] == '\0') {
					LOG_ERROR("missing config path after -c");
					free(inline_routes);
					return 2;
				}
				conf_path = optarg;
				break;

			case 'L':
				if (optarg == NULL || optarg[0] == '\0') {
					LOG_ERROR("missing route after -L");
					free(inline_routes);
					return 2;
				}

				if (inline_route_count == inline_route_cap) {
					size_t new_cap = inline_route_cap == 0 ? 4 : inline_route_cap * 2;
					const char **new_routes = realloc(inline_routes,
							new_cap * sizeof(*inline_routes));

					if (new_routes == NULL) {
						LOG_ERROR("inline route realloc failed",
								"err", _LOGV(strerror(errno)));
						free(inline_routes);
						return 1;
					}

					inline_routes = new_routes;
					inline_route_cap = new_cap;
				}

				inline_routes[inline_route_count++] = optarg;
				break;

			case 'h':
				usage(stdout, argv[0]);
				free(inline_routes);
				return 0;

			case 'v':
				fprintf(stdout, "tinyproxy v0.2.0\n");
				free(inline_routes);
				return 0;

			default:
				usage(stderr, argv[0]);
				free(inline_routes);
				return 2;
		}
	}

	if(conf_path == NULL && inline_route_count == 0) {
		usage(stdout, argv[0]);
		free(inline_routes);
		return 0;
	}

	if (conf_path && inline_route_count > 0) {
		LOG_ERROR("-c and -L cannot be used together");
		usage(stderr, argv[0]);
		free(inline_routes);
		return 2;
	}

	if (optind != argc) {
		LOG_ERROR("unexpected argument", "argv", _LOGV(argv[optind]));
		usage(stderr, argv[0]);
		return 2;
	}

	if (inline_route_count > 0) {
		routes = calloc(inline_route_count, sizeof(*routes));
		if (routes == NULL) {
			LOG_ERROR("route calloc failed", "err", _LOGV(strerror(errno)));
			goto out;
		}

		for (size_t i = 0; i < inline_route_count; i++) {
			char *line = strdup(inline_routes[i]);
			if (line == NULL) {
				LOG_ERROR("inline route strdup failed", "err", _LOGV(strerror(errno)));
				rc = -ENOMEM;
				goto out;
			}

			struct route *r = &routes[route_count];
			r->line_no = (unsigned int)i + 1;

			rc = parse_route_line(line, r);
			free(line);

			if (rc != 0) {
				LOG_ERROR("failed to load inline route",
						"line", _LOGV(r->line_no),
						"route", _LOGV(inline_routes[i]),
						"err", _LOGV(strerror(-rc)));
				goto out;
			}

			route_count++;
		}
	} else {
		rc = load_routes_from_file(conf_path, &routes, &route_count);
		if (rc != 0) {
			LOG_ERROR("failed to load config",
					"path", _LOGV(conf_path),
					"err", _LOGV(strerror(-rc)));
			goto out;
		}
	}

	if (route_count == 0) {
		LOG_ERROR("config has no routes", "path", _LOGV(conf_path));
		goto out;
	}

	route_ctxs = calloc(route_count, sizeof(*route_ctxs));
	if (route_ctxs == NULL) {
		LOG_ERROR("route calloc failed", "err", _LOGV(strerror(errno)));
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

	struct worker_pool wpool;
	size_t worker_count = 1;

	rc = worker_pool_init(&wpool, worker_count);
	if (rc != 0) {
		LOG_ERROR("failed to init worker pool", "err", _LOGV(rc));
		goto out;
	}
	worker_pool_ready = true;

	rc = worker_pool_start(&wpool);
	if (rc != 0) {
		LOG_ERROR("failed to start worker pool", "err", _LOGV(rc));
		goto out;
	}
	worker_pool_started = true;

	for (size_t i = 0; i < route_count; i++) {
		const struct route *r = &routes[i];

		rc = validate_route(r);
		if (rc != 0) {
			LOG_ERROR("invalid route",
					"line", _LOGV(r->line_no),
					"listen", _LOGV_ENDPOINT(&r->listen),
					"upstream", _LOGV_ENDPOINT(&r->upstream)
					);
			goto out;
		}

		rc = start_route(base, &wpool, r, &route_ctxs[route_ctx_count]);
		if (rc == 0) {
			route_ctx_count++;
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
	for (size_t i = 0; i < route_ctx_count; i++) {
		stop_route(&route_ctxs[i]);
	}

	if (worker_pool_started) {
		worker_pool_stop(&wpool);
		worker_pool_join(&wpool);
	}

	if (worker_pool_ready) {
		worker_pool_free(&wpool);
	}

	if (signals_started) {
		free_signal_handlers(&signals);
	}

	free(route_ctxs);

	if (base != NULL) {
		event_base_free(base);
	}

	free(routes);
	free(inline_routes);

#ifdef _WIN32
	if (wsa_started) {
		WSACleanup();
	}
#endif

	return exit_code;
}
