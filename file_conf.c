#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#include "klog.h"
#include "file_conf.h"

#define MAX_LINE_LEN 512

static char *trim(char *s)
{
	while (isspace((unsigned char)*s)) {
		s++;
	}

	if (*s == '\0') {
		return s;
	}

	char *end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end)) {
		*end = '\0';
		end--;
	}

	return s;
}

static int parse_port(const char *s, uint16_t *out)
{
	char *end = NULL;
	errno = 0;

	unsigned long v = strtoul(s, &end, 10);

	if (errno != 0 || *s == '\0' || *end != '\0' || v == 0 || v > 65535) {
		return -1;
	}

	*out = (uint16_t)v;
	return 0;
}

static int split_host_port(const char *input,
						   char *host, size_t host_len,
						   uint16_t *port)
{
	const char *colon = strrchr(input, ':');
	if (!colon) {
		return -1;
	}

	size_t hlen = (size_t)(colon - input);
	const char *port_s = colon + 1;

	if (hlen >= host_len || *port_s == '\0') {
		return -1;
	}

	memcpy(host, input, hlen);
	host[hlen] = '\0';

	if (parse_port(port_s, port) != 0) {
		return -1;
	}

	// Allow ":80" as shorthand for all interfaces.
	if (host[0] == '\0') {
		snprintf(host, host_len, "0.0.0.0");
	}

	return 0;
}

static int parse_proto(const char *s, enum proto *out)
{
	if (strcmp(s, "tcp") == 0) {
		*out = PROTO_TCP;
		return 0;
	}

	if (strcmp(s, "udp") == 0) {
		*out = PROTO_UDP;
		return 0;
	}

	return -1;
}

static int parse_positive_int(const char *s, int *out)
{
	char *end = NULL;
	long v;

	if (s == NULL || *s == '\0') {
		return -EINVAL;
	}

	errno = 0;
	v = strtol(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0') {
		return -EINVAL;
	}

	if (v <= 0 || v > INT_MAX) {
		return -ERANGE;
	}

	*out = (int)v;
	return 0;
}

static int parse_route_options(char *s, struct route_options *opts)
{
	char *saveptr = NULL;
	char *tok;

	if (s == NULL || *s == '\0') {
		return 0;
	}

#define PARSE_INT_OPT(name, field) do { \
		if (strcmp(key, name) == 0) { \
			int rc = parse_positive_int(value, &opts->field); \
			if (rc != 0) { \
				return rc; \
			} \
			goto next_option; \
		} \
	} while (0)

#define PARSE_BOOL_OPT(name, field) do { \
		if (strcmp(tok, name) == 0) { \
			opts->field = true; \
			goto next_option; \
		} \
	} while (0)

	for (tok = strtok_r(s, ",", &saveptr);
	     tok != NULL;
	     tok = strtok_r(NULL, ",", &saveptr)) {
		char *eq;

		tok = trim(tok);
		if (*tok == '\0') {
			return -EINVAL;
		}

		eq = strchr(tok, '=');

		if (eq != NULL) {
			const char *key;
			const char *value;

			*eq = '\0';
			key = trim(tok);
			value = trim(eq + 1);

			if (*key == '\0' || *value == '\0') {
				return -EINVAL;
			}

			PARSE_INT_OPT("idle_timeout", idle_timeout_sec);
			PARSE_INT_OPT("connect_timeout", connect_timeout_sec);

			return -EINVAL;
		}

		PARSE_BOOL_OPT("proxy_v2", proxy_v2);
		PARSE_BOOL_OPT("keep_alive", keep_alive);

		return -EINVAL;

next_option:
		continue;
	}

#undef PARSE_BOOL_OPT
#undef PARSE_INT_OPT

	return 0;
}

static inline void route_options_set_defaults(struct route_options *opts)
{
	opts->proxy_v2 = false;
	opts->keep_alive = false;
	opts->idle_timeout_sec = ROUTE_DEFAULT_IDLE_TIMEOUT_SEC;
	opts->connect_timeout_sec = ROUTE_DEFAULT_CONNECT_TIMEOUT_SEC;
}

static int parse_endpoint(const char *s, struct endpoint *ep)
{
	const char *path;

	memset(ep, 0, sizeof(*ep));

	if (strncmp(s, "unix:", 5) == 0) {
		path = s + 5;

		if (path[0] == '\0') {
			return -1;
		}

		if (strlen(path) >= sizeof(ep->path)) {
			return -1;
		}

		ep->kind = ENDPOINT_UNIX;
		strcpy(ep->path, path);
		return 0;
	} else if (strncmp(s, "unix-dgram:", 11) == 0) {
		path = s + 11;

		if (path[0] == '\0') {
			return -1;
		}

		if (strlen(path) >= sizeof(ep->path)) {
			return -1;
		}

		ep->kind = ENDPOINT_UNIX_DGRAM;
		strcpy(ep->path, path);
		return 0;
	}

	ep->kind = ENDPOINT_INET;

	if (split_host_port(s, ep->host, sizeof(ep->host), &ep->port) != 0) {
		return -1;
	}

	return 0;
}

static int parse_route_line(char *line, struct route *route)
{
	char *fields[4] = {0};
	size_t count = 0;
	char *saveptr = NULL;
	char *tok;

	tok = strtok_r(line, " \t\r\n", &saveptr);
	while (tok != NULL) {
		if (count >= 4) {
			return -1; // too many fields
		}

		fields[count++] = tok;
		tok = strtok_r(NULL, " \t\r\n", &saveptr);
	}

	if (count < 3) {
		return -1;
	}

	memset(route, 0, sizeof(*route));
	route_options_set_defaults(&route->opts);

	if (parse_endpoint(fields[0], &route->listen) != 0) {
		return -1;
	}

	if (parse_endpoint(fields[1], &route->upstream) != 0) {
		return -1;
	}

	if (parse_proto(fields[2], &route->proto) != 0) {
		return -1;
	}

	if (count == 4 && parse_route_options(fields[3], &route->opts) != 0) {
		return -1;
	}

	return 0;
}

int load_routes_from_file(const char *path, struct route **routes_out, size_t *count_out)
{
	FILE *fp = fopen(path, "r");
	if (!fp) {
		return -errno;
	}

	struct route *routes = NULL;
	size_t count = 0;
	size_t cap = 0;

	char buf[MAX_LINE_LEN];
	unsigned int line_no = 0;

	while (fgets(buf, sizeof(buf), fp)) {
		line_no++;

		// Reject overlong lines.
		if (!strchr(buf, '\n') && !feof(fp)) {
			fclose(fp);
			free(routes);
			return -E2BIG;
		}

		char *line = trim(buf);

		if (*line == '\0' || *line == '#') {
			continue;
		}

		// Strip inline comments.
		char *hash = strchr(line, '#');
		if (hash) {
			*hash = '\0';
			line = trim(line);
			if (*line == '\0') {
				continue;
			}
		}

		if (count == cap) {
			size_t new_cap = cap ? cap * 2 : 8;
			struct route *new_routes = realloc(routes, new_cap * sizeof(*routes));
			if (!new_routes) {
				fclose(fp);
				free(routes);
				return -ENOMEM;
			}

			routes = new_routes;
			cap = new_cap;
		}

		char original[MAX_LINE_LEN];
		snprintf(original, sizeof(original), "%s", line);

		struct route *r = &routes[count];
		if (parse_route_line(line, r) != 0) {
			LOG_ERROR("invalid route config",
					"path", _LOGV(path),
					"line", _LOGV(line_no),
					"text", _LOGV(original));
			fclose(fp);
			free(routes);
			return -EINVAL;
		}

		r->line_no = line_no;
		count++;
	}

	if (ferror(fp)) {
		int err = errno;
		fclose(fp);
		free(routes);
		return -err;
	}

	fclose(fp);

	*routes_out = routes;
	*count_out = count;

	return 0;
}

void free_routes(struct route *routes)
{
	free(routes);
}
