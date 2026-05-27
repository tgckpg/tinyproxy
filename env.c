#include "env.h"

#include <stdlib.h>

#define TINYPROXY_DEFAULT_RUNTIME_DIR "/tmp/tinyproxy"

const char *tinyproxy_runtime_dir(void)
{
	const char *v = getenv("TINYPROXY_RUNTIME_DIR");

	if (v != NULL && v[0] != '\0') {
		return v;
	}

	return TINYPROXY_DEFAULT_RUNTIME_DIR;
}
