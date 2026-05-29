#ifndef FILE_CONF_H
#define FILE_CONF_H

#include <stddef.h>
#include <stdbool.h>

#include "route.h"

int parse_route_line(char *line, struct route *route);

int load_routes_from_text(
	const char *name,
	const char *data,
	size_t data_len,
	struct route **routes_out,
	size_t *count_out
);

int load_routes_from_file(const char *path, struct route **routes_out, size_t *count_out);

#endif
