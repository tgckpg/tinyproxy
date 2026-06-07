#ifndef STREAM_SNIFF_H
#define STREAM_SNIFF_H

#include <stdbool.h>
#include <stddef.h>

#include <event2/buffer.h>

#define STREAM_SNIFF_MAX 8192
#define STREAM_SNI_MAX 253

enum stream_sniff_status {
	STREAM_SNIFF_NOT_OBSERVED = 0,
	STREAM_SNIFF_NEED_MORE,
	STREAM_SNIFF_PARSED,
	STREAM_SNIFF_MISSING,
	STREAM_SNIFF_NOT_TLS,
	STREAM_SNIFF_TRUNCATED,
	STREAM_SNIFF_NAME_TOO_LONG,
	STREAM_SNIFF_PARSE_ERROR,
};

struct stream_sniff {
	char sni[STREAM_SNI_MAX + 1];
	enum stream_sniff_status status;
	size_t observed;
	bool done;
};

void stream_sniff_peek_client_input(
	struct stream_sniff *s,
	struct evbuffer *input
);

const char *stream_sniff_log_sni(const struct stream_sniff *s);

#endif
