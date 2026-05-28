#ifndef FILE_CONF_H
#define FILE_CONF_H

#include <stddef.h>
#include <stdbool.h>

#include "route.h"

int parse_route_line(char *line, struct route *route);
int load_routes_from_file(const char *path, struct route **routes_out, size_t *count_out);
void free_routes(struct route *routes);

#endif
