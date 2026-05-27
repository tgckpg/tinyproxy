#include "x_builtins.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int x_builtin_parse(const char *s, enum x_builtin_upstream *out)
{
	if (s == NULL || out == NULL) {
		return -EINVAL;
	}

	if (strcmp(s, "client_addr") == 0 || strcmp(s, "X_CLIENT_ADDR") == 0) {
		*out = X_BUILTIN_CLIENT_ADDR;
		return 0;
	}

	if (strcmp(s, "discard") == 0 || strcmp(s, "X_DISCARD") == 0) {
		*out = X_BUILTIN_DISCARD;
		return 0;
	}

	if (strcmp(s, "hang") == 0 || strcmp(s, "X_HANG") == 0) {
		*out = X_BUILTIN_HANG;
		return 0;
	}

	if (strcmp(s, "close") == 0 || strcmp(s, "X_CLOSE") == 0) {
		*out = X_BUILTIN_CLOSE;
		return 0;
	}

	return -EINVAL;
}

const char *x_builtin_name(enum x_builtin_upstream builtin)
{
	switch (builtin) {
	case X_BUILTIN_CLIENT_ADDR:
		return "client_addr";
	case X_BUILTIN_DISCARD:
		return "discard";
	case X_BUILTIN_HANG:
		return "hang";
	case X_BUILTIN_CLOSE:
		return "close";
	case X_BUILTIN_NONE:
	default:
		return "none";
	}
}

int x_builtin_handle(
	const struct x_builtin_request *req,
	struct x_builtin_response *res
) {
	int n;

	if (req == NULL || res == NULL) {
		return -EINVAL;
	}

	memset(res, 0, sizeof(*res));

	switch (req->builtin) {
	case X_BUILTIN_CLIENT_ADDR:
		if (req->client_addr == NULL) {
			return -EINVAL;
		}

		n = snprintf(res->data, sizeof(res->data), "%s\n", req->client_addr);
		if (n < 0 || (size_t)n >= sizeof(res->data)) {
			return -ENOSPC;
		}

		res->data_len = (size_t)n;
		res->action = X_BUILTIN_ACTION_CLOSE;
		return 0;

	case X_BUILTIN_DISCARD:
		res->action = X_BUILTIN_ACTION_DISCARD;
		return 0;

	case X_BUILTIN_CLOSE:
		res->data_len = 0;
		res->action = X_BUILTIN_ACTION_CLOSE;
		return 0;

	case X_BUILTIN_HANG:
		res->action = X_BUILTIN_ACTION_HANG;
		return 0;

	case X_BUILTIN_NONE:
	default:
		return -EINVAL;
	}
}
