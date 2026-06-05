#ifndef X_BUILTINS_H
#define X_BUILTINS_H

#include <stddef.h>
#include <sys/types.h>

enum x_builtin_upstream {
	X_BUILTIN_NONE = 0,
	X_BUILTIN_CLIENT_ADDR,
	X_BUILTIN_DISCARD,
	X_BUILTIN_HANG,
	X_BUILTIN_HTTP_OK,
	X_BUILTIN_LOG_CONN,
	X_BUILTIN_CLOSE,
};

enum x_builtin_action {
	X_BUILTIN_ACTION_CLOSE,
	X_BUILTIN_ACTION_DISCARD,
	X_BUILTIN_ACTION_HANG,
	X_BUILTIN_ACTION_HTTP_RESP,
};

struct x_builtin_request {
	enum x_builtin_upstream builtin;
	const char *client_addr;
};

struct x_builtin_response {
	enum x_builtin_action action;
	char data[256];
	size_t data_len;
};

int x_builtin_parse(const char *s, enum x_builtin_upstream *out);
const char *x_builtin_name(enum x_builtin_upstream builtin);

int x_builtin_handle(
	const struct x_builtin_request *req,
	struct x_builtin_response *res
);

#endif
