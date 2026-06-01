#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#include "klog.h"
#include "endpoint.h"
#include "file_conf.h"

#define MAX_LINE_LEN 512
#define MAX_ROUTE_FIELDS 32

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

static bool same_listen_endpoint(
	const struct endpoint *a,
	const struct endpoint *b
) {
	if (a->proto != b->proto) {
		return false;
	}

	switch (a->proto) {
	case PROTO_TCP:
	case PROTO_UDP:
		return strcmp(a->host, b->host) == 0 &&
			a->port == b->port;

	case PROTO_UNIX_STREAM:
	case PROTO_UNIX_DGRAM:
		return strcmp(a->path, b->path) == 0;

	default:
		return false;
	}
}

static int find_conflicting_route(
	const struct route *routes,
	size_t count,
	const struct route *new_route
) {
	for (size_t i = 0; i < count; i++) {
		if (same_listen_endpoint(&routes[i].listen, &new_route->listen)) {
			return (int)i;
		}
	}

	return -1;
}

static int split_host_port(const char *input,
						   char *host, size_t host_len,
						   uint16_t *port,
						   bool *is_ipv6)
{
	const char *port_s;
	size_t hlen;

	if (!input || !host || host_len == 0 || !port || !is_ipv6) {
		return -1;
	}

	host[0] = '\0';
	*is_ipv6 = false;

	if (input[0] == '[') {
		const char *close = strchr(input, ']');

		if (!close) {
			return -1;
		}

		if (close[1] != ':') {
			return -1;
		}

		hlen = (size_t)(close - (input + 1));
		port_s = close + 2;

		if (hlen == 0 || hlen >= host_len || *port_s == '\0') {
			return -1;
		}

		memcpy(host, input + 1, hlen);
		host[hlen] = '\0';
		*is_ipv6 = true;
	} else {
		const char *first_colon = strchr(input, ':');
		const char *last_colon = strrchr(input, ':');

		if (!last_colon) {
			return -1;
		}

		/*
		 * Reject bare IPv6 with port.
		 * Use [::1]:80 instead.
		 */
		if (first_colon != last_colon) {
			return -1;
		}

		hlen = (size_t)(last_colon - input);
		port_s = last_colon + 1;

		if (hlen >= host_len || *port_s == '\0') {
			return -1;
		}

		memcpy(host, input, hlen);
		host[hlen] = '\0';

		/* Allow ":80" as IPv4 wildcard shorthand. */
		if (host[0] == '\0') {
			snprintf(host, host_len, "0.0.0.0");
		}
	}

	if (parse_port(port_s, port) != 0) {
		return -1;
	}

	return 0;
}

static int parse_proto(const char *s, enum endpoint_proto *out)
{
	if (strcmp(s, "tcp") == 0) {
		*out = PROTO_TCP;
		return 0;
	}

	if (strcmp(s, "udp") == 0) {
		*out = PROTO_UDP;
		return 0;
	}

	if (strcmp(s, "unix") == 0) {
		*out = PROTO_UNIX_STREAM;
		return 0;
	}

	if (strcmp(s, "unix-dgram") == 0) {
		*out = PROTO_UNIX_DGRAM;
		return 0;
	}

	if (strcmp(s, "file") == 0) {
		*out = PROTO_FILE;
		return 0;
	}

	if (strcmp(s, "builtin") == 0) {
		*out = PROTO_BUILTIN;
		return 0;
	}

	return -1;
}

static int parse_broadcast_reply_mode(
	const char *value,
	enum broadcast_reply_mode *out)
{
	if (!value || !out) {
		return -EINVAL;
	}

	if (strcmp(value, "listen") == 0) {
		*out = BROADCAST_REPLY_LISTEN;
		return 0;
	}

	if (strcmp(value, "upstream") == 0) {
		*out = BROADCAST_REPLY_UPSTREAM;
		return 0;
	}

	return -EINVAL;
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

#define PARSE_ENUM_OPT(name, parser, field) do { \
		if (strcmp(key, name) == 0) { \
			int rc = parser(value, &opts->field); \
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
			PARSE_ENUM_OPT("broadcast_reply",
				parse_broadcast_reply_mode,
				broadcast_reply);

			return -EINVAL;
		}

		PARSE_BOOL_OPT("proxy_v2", proxy_v2);
		PARSE_BOOL_OPT("keep_alive", keep_alive);

		if (strcmp(tok, "broadcast_reply") == 0) {
			opts->broadcast_reply = BROADCAST_REPLY_LISTEN;
			goto next_option;
		}

		return -EINVAL;

next_option:
		continue;
	}

#undef PARSE_BOOL_OPT
#undef PARSE_ENUM_OPT
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

static int parse_endpoint(enum endpoint_proto proto, const char *s, struct endpoint *ep)
{
	const char *path;

	memset(ep, 0, sizeof(*ep));
	ep->proto = proto;

	switch (proto) {
	case PROTO_TCP:
	case PROTO_UDP: {
		bool is_ipv6 = false;

		if (split_host_port(s, ep->host, sizeof(ep->host), &ep->port, &is_ipv6) != 0) {
			return -1;
		}

		ep->kind = is_ipv6 ? ENDPOINT_INET6 : ENDPOINT_INET;
		return 0;
	}

	case PROTO_UNIX_STREAM:
		path = s;

		if (path[0] == '\0' || strlen(path) >= sizeof(ep->path)) {
			return -1;
		}

		ep->kind = ENDPOINT_UNIX;
		strcpy(ep->path, path);
		return 0;

	case PROTO_UNIX_DGRAM:
		path = s;

		if (path[0] == '\0' || strlen(path) >= sizeof(ep->path)) {
			return -1;
		}

		ep->kind = ENDPOINT_UNIX_DGRAM;
		strcpy(ep->path, path);
		return 0;

	case PROTO_FILE:
		path = s;

		if (path[0] == '\0') {
			LOG_ERROR("missing file path");
			return -1;
		}

		if (strlen(path) >= sizeof(ep->path)) {
			LOG_ERROR("file path too long",
				"max", _LOGV(sizeof(ep->path) - 1)
			);
			return -1;
		}

		ep->kind = ENDPOINT_FILE;
		strcpy(ep->path, path);
		return 0;

	case PROTO_BUILTIN: {
		enum x_builtin_upstream builtin;
		const char *name = s;

		if (name[0] == '\0' || x_builtin_parse(name, &builtin) < 0) {
			return -1;
		}

		ep->kind = ENDPOINT_BUILTIN;
		ep->builtin = builtin;
		return 0;
	}
	}

	return -1;
}

int parse_route_line(char *line, struct route *route)
{
	char *fields[MAX_ROUTE_FIELDS] = {0};
	size_t count = 0;
	size_t pos = 0;
	char *saveptr = NULL;
	char *tok;
	enum endpoint_proto listen_proto;
	enum endpoint_proto backend_proto;

	tok = strtok_r(line, " \t\r\n", &saveptr);
	while (tok != NULL) {
		if (count >= MAX_ROUTE_FIELDS) {
			return -1;
		}

		fields[count++] = tok;
		tok = strtok_r(NULL, " \t\r\n", &saveptr);
	}

	if (count == 0) {
		return -1;
	}

	/* Config file form:
	 *   listen tcp 0.0.0.0:80 tcp 10.0.1.1:80 proxy_v2
	 *
	 * Also accept CLI-short form:
	 *   tcp 0.0.0.0:80 tcp 10.0.1.1:80 proxy_v2
	 */
	if (strcmp(fields[0], "listen") == 0) {
		pos = 1;
	}

	if (count - pos < 4) {
		return -1;
	}

	memset(route, 0, sizeof(*route));
	route_options_set_defaults(&route->opts);

	if (parse_proto(fields[pos], &listen_proto) != 0) {
		return -1;
	}
	pos++;

	if (parse_endpoint(listen_proto, fields[pos], &route->listen) != 0) {
		return -1;
	}
	pos++;

	if (parse_proto(fields[pos], &backend_proto) != 0) {
		return -1;
	}
	pos++;

	if (parse_endpoint(backend_proto, fields[pos], &route->upstream) != 0) {
		return -1;
	}
	pos++;

	for (; pos < count; pos++) {
		if (parse_route_options(fields[pos], &route->opts) != 0) {
			return -1;
		}
	}

	return 0;
}

int load_routes_from_text(
	const char *name,
	const char *data,
	size_t data_len,
	struct route **routes_out,
	size_t *count_out
) {
	struct route *routes = NULL;
	size_t count = 0;
	size_t cap = 0;

	size_t pos = 0;
	unsigned int line_no = 0;

	*routes_out = NULL;
	*count_out = 0;

	while (pos < data_len) {
		line_no++;

		const char *line_start = data + pos;
		const void *nlp = memchr(line_start, '\n', data_len - pos);

		size_t raw_len;
		if (nlp) {
			raw_len = (const char *)nlp - line_start;
			pos += raw_len + 1;
		} else {
			raw_len = data_len - pos;
			pos = data_len;
		}

		/* Handle CRLF input nicely. */
		if (raw_len > 0 && line_start[raw_len - 1] == '\r') {
			raw_len--;
		}

		/*
		 * Need room for NUL.
		 *
		 * This keeps the same basic safety property as the fgets()
		 * version: no parser call ever sees an overlarge line.
		 */
		if (raw_len >= MAX_LINE_LEN) {
			free(routes);
			return -E2BIG;
		}

		char buf[MAX_LINE_LEN];
		memcpy(buf, line_start, raw_len);
		buf[raw_len] = '\0';

		char *line = trim(buf);

		if (*line == '\0' || *line == '#') {
			continue;
		}

		/* Strip inline comments. */
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

			if (new_cap > SIZE_MAX / sizeof(*routes)) {
				free(routes);
				return -EOVERFLOW;
			}

			struct route *new_routes = realloc(routes, new_cap * sizeof(*routes));
			if (!new_routes) {
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
				"name", _LOGV(name ? name : "<memory>"),
				"line", _LOGV(line_no),
				"text", _LOGV(original)
			);

			free(routes);
			return -EINVAL;
		}

		r->line_no = line_no;

		int conflict_idx = find_conflicting_route(routes, count, r);
		if (conflict_idx >= 0) {
			const struct route *old = &routes[conflict_idx];

			LOG_ERROR("conflicting route config",
				"name", _LOGV(name ? name : "<memory>"),
				"line", _LOGV(line_no),
				"conflicts_with_line", _LOGV(old->line_no),
				"listen", _LOGV_ENDPOINT(&r->listen),
				"text", _LOGV(original)
			);

			free(routes);
			return -EADDRINUSE;
		}

		count++;
	}

	*routes_out = routes;
	*count_out = count;

	return 0;
}

int load_routes_from_file(const char *path, struct route **routes_out, size_t *count_out)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		return -errno;
	}

	if (fseek(fp, 0, SEEK_END) != 0) {
		int err = errno;
		fclose(fp);
		return -err;
	}

	long file_size = ftell(fp);
	if (file_size < 0) {
		int err = errno;
		fclose(fp);
		return -err;
	}

	if (fseek(fp, 0, SEEK_SET) != 0) {
		int err = errno;
		fclose(fp);
		return -err;
	}

	char *data = NULL;

	if (file_size > 0) {
		data = malloc((size_t)file_size);
		if (!data) {
			fclose(fp);
			return -ENOMEM;
		}

		size_t nread = fread(data, 1, (size_t)file_size, fp);
		if (nread != (size_t)file_size) {
			int err = ferror(fp) ? errno : EIO;
			free(data);
			fclose(fp);
			return -err;
		}
	}

	fclose(fp);

	int rc = load_routes_from_text(
		path,
		data ? data : "",
		(size_t)file_size,
		routes_out,
		count_out
	);

	free(data);
	return rc;
}
