#include "env.h"

#include <stdlib.h>

#ifdef _WIN32
#define TINYPROXY_DEFAULT_RUNTIME_DIR "tinyproxy"
#else
#define TINYPROXY_DEFAULT_RUNTIME_DIR "/tmp/tinyproxy"
#endif

const char *tinyproxy_runtime_dir(void)
{
	const char *v;

	v = getenv("TINYPROXY_RUNTIME_DIR");
	if (v != NULL && v[0] != '\0') {
		return v;
	}

#ifndef _WIN32
	return TINYPROXY_DEFAULT_RUNTIME_DIR;
#else
	v = getenv("TEMP");
	if (v != NULL && v[0] != '\0') {
		return v;
	}

	v = getenv("TMP");
	if (v != NULL && v[0] != '\0') {
		return v;
	}

	return ".";
#endif
}
