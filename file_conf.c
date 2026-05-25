#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>

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

static int parse_route_line(char *line, struct route *route)
{
	char *fields[4] = {0};
	size_t count = 0;

	char *tok = strtok(line, " \t");
	while (tok && count < 4) {
		fields[count++] = tok;
		tok = strtok(NULL, " \t");
	}

	if (tok != NULL) {
		return -1; // too many fields
	}

	if (count < 3) {
		return -1;
	}

	memset(route, 0, sizeof(*route));

	if (split_host_port(fields[0],
						route->listen_host, sizeof(route->listen_host),
						&route->listen_port) != 0) {
		return -1;
	}

	if (split_host_port(fields[1],
						route->upstream_host, sizeof(route->upstream_host),
						&route->upstream_port) != 0) {
		return -1;
	}

	if (parse_proto(fields[2], &route->proto) != 0) {
		return -1;
	}

	route->send_proxy_v2 = false;

	if (count == 4) {
		if (strcmp(fields[3], "proxyv2") != 0) {
			return -1;
		}

		route->send_proxy_v2 = true;
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

		if (parse_route_line(line, &routes[count]) != 0) {
			fprintf(stderr, "%s:%u: invalid route config\n", path, line_no);
			fclose(fp);
			free(routes);
			return -EINVAL;
		}

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
