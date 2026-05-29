#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "route.h"
#include "file_conf.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct route *routes = NULL;
	size_t count = 0;

	int rc = load_routes_from_text(
		"fuzzer",
		(const char *)data,
		size,
		&routes,
		&count
	);

	if (rc == 0) {
		if (count > 0 && routes == NULL) {
			abort();
		}

		free(routes);
		return 0;
	}

	if (routes != NULL) {
		abort();
	}

	return 0;
}
