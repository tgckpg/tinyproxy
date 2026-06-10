#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include <event2/buffer.h>

#include "stream_sniff.h"

static void check_invariants(const struct stream_sniff *s)
{
	if (s->sni[STREAM_SNI_MAX] != '\0') {
		abort();
	}

	if (strlen(s->sni) > STREAM_SNI_MAX) {
		abort();
	}

	if (s->observed > STREAM_SNIFF_MAX) {
		abort();
	}

	if (s->done) {
		switch (s->status) {
		case STREAM_SNIFF_PARSED:
		case STREAM_SNIFF_MISSING:
		case STREAM_SNIFF_NOT_TLS:
		case STREAM_SNIFF_TRUNCATED:
		case STREAM_SNIFF_NAME_TOO_LONG:
		case STREAM_SNIFF_PARSE_ERROR:
			break;

		case STREAM_SNIFF_NOT_OBSERVED:
		case STREAM_SNIFF_NEED_MORE:
		default:
			abort();
		}
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct evbuffer *input;
	struct stream_sniff s;

	memset(&s, 0, sizeof(s));

	input = evbuffer_new();
	if (input == NULL) {
		return 0;
	}

	if (size > 0 && evbuffer_add(input, data, size) != 0) {
		evbuffer_free(input);
		return 0;
	}

	stream_sniff_peek_client_input(&s, input);

	/*
	 * Also fuzz the log helper after parsing.
	 * This catches cases where status/sni state is inconsistent.
	 */
	(void)stream_sniff_log_sni(&s);

	check_invariants(&s);

	evbuffer_free(input);
	return 0;
}
