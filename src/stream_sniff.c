#include "stream_sniff.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "klog.h"

static uint16_t read_u16_be(const unsigned char *p)
{
	return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t read_u24_be(const unsigned char *p)
{
	return ((uint32_t)p[0] << 16) |
		((uint32_t)p[1] << 8) |
		(uint32_t)p[2];
}

static bool valid_sni_name(const unsigned char *p, size_t len)
{
	size_t i;

	if (len == 0 || len > STREAM_SNI_MAX) {
		return false;
	}

	/*
	 * Keep validation conservative but not overly clever.
	 *
	 * SNI host_name is supposed to be a DNS hostname, not an arbitrary
	 * string. Do not accept NUL/control bytes, slashes, spaces, etc.
	 */
	for (i = 0; i < len; i++) {
		unsigned char c = p[i];

		if (isalnum(c) || c == '-' || c == '.') {
			continue;
		}

		return false;
	}

	return true;
}

static enum stream_sniff_status parse_sni_from_client_hello(
	const unsigned char *buf,
	size_t len,
	char *out,
	size_t out_len,
	bool *need_more)
{
	size_t pos;
	size_t record_len;
	size_t record_end;
	size_t hs_len;
	size_t hs_end;
	size_t session_id_len;
	size_t cipher_suites_len;
	size_t compression_methods_len;
	size_t extensions_len;
	size_t extensions_end;

	*need_more = false;

	if (len == 0) {
		*need_more = true;
		return STREAM_SNIFF_NEED_MORE;
	}

	/*
	 * TLS record header:
	 *
	 *   content_type:     1 byte  == 22 handshake
	 *   legacy_version:   2 bytes
	 *   length:           2 bytes
	 */
	if (buf[0] != 0x16) {
		return STREAM_SNIFF_NOT_TLS;
	}

	if (len < 5) {
		*need_more = true;
		return STREAM_SNIFF_NEED_MORE;
	}

	/*
	 * TLS major version should be 3 for SSLv3/TLS 1.x records.
	 * TLS 1.3 ClientHello commonly still uses legacy record version 0x0301
	 * or 0x0303, so do not require an exact minor version.
	 */
	if (buf[1] != 0x03) {
		return STREAM_SNIFF_PARSE_ERROR;
	}

	record_len = read_u16_be(buf + 3);
	record_end = 5 + record_len;

	if (record_len == 0) {
		return STREAM_SNIFF_PARSE_ERROR;
	}

	if (record_end > len) {
		*need_more = true;
		return STREAM_SNIFF_NEED_MORE;
	}

	pos = 5;

	/*
	 * Handshake message:
	 *
	 *   handshake_type:   1 byte  == 1 ClientHello
	 *   length:           3 bytes
	 */
	if (pos + 4 > record_end) {
		return STREAM_SNIFF_PARSE_ERROR;
	}

	if (buf[pos] != 0x01) {
		return STREAM_SNIFF_NOT_TLS;
	}

	hs_len = read_u24_be(buf + pos + 1);
	pos += 4;
	hs_end = pos + hs_len;

	if (hs_len == 0 || hs_end > record_end) {
		return STREAM_SNIFF_PARSE_ERROR;
	}

	/*
	 * ClientHello body:
	 *
	 *   legacy_version:              2 bytes
	 *   random:                     32 bytes
	 *   session_id_len:              1 byte
	 *   session_id:                  variable
	 *   cipher_suites_len:           2 bytes
	 *   cipher_suites:               variable
	 *   compression_methods_len:      1 byte
	 *   compression_methods:          variable
	 *   extensions_len:               2 bytes
	 *   extensions:                   variable
	 */
	if (pos + 2 + 32 + 1 > hs_end) {
		return STREAM_SNIFF_PARSE_ERROR;
	}

	pos += 2;  /* legacy_version */
	pos += 32; /* random */

	session_id_len = buf[pos];
	pos += 1;

	if (pos + session_id_len > hs_end) {
		return STREAM_SNIFF_PARSE_ERROR;
	}
	pos += session_id_len;

	if (pos + 2 > hs_end) {
		return STREAM_SNIFF_PARSE_ERROR;
	}

	cipher_suites_len = read_u16_be(buf + pos);
	pos += 2;

	if (cipher_suites_len == 0 || (cipher_suites_len % 2) != 0) {
		return STREAM_SNIFF_PARSE_ERROR;
	}

	if (pos + cipher_suites_len > hs_end) {
		return STREAM_SNIFF_PARSE_ERROR;
	}
	pos += cipher_suites_len;

	if (pos + 1 > hs_end) {
		return STREAM_SNIFF_PARSE_ERROR;
	}

	compression_methods_len = buf[pos];
	pos += 1;

	if (compression_methods_len == 0) {
		return STREAM_SNIFF_PARSE_ERROR;
	}

	if (pos + compression_methods_len > hs_end) {
		return STREAM_SNIFF_PARSE_ERROR;
	}
	pos += compression_methods_len;

	/*
	 * No extensions means no SNI.
	 */
	if (pos == hs_end) {
		return STREAM_SNIFF_MISSING;
	}

	if (pos + 2 > hs_end) {
		return STREAM_SNIFF_PARSE_ERROR;
	}

	extensions_len = read_u16_be(buf + pos);
	pos += 2;

	extensions_end = pos + extensions_len;
	if (extensions_end != hs_end) {
		return STREAM_SNIFF_PARSE_ERROR;
	}

	while (pos + 4 <= extensions_end) {
		uint16_t ext_type;
		uint16_t ext_len;
		size_t ext_data;
		size_t ext_end;

		ext_type = read_u16_be(buf + pos);
		ext_len = read_u16_be(buf + pos + 2);
		pos += 4;

		ext_data = pos;
		ext_end = ext_data + ext_len;

		if (ext_end > extensions_end) {
			return STREAM_SNIFF_PARSE_ERROR;
		}

		/*
		 * Extension type 0 = server_name.
		 */
		if (ext_type == 0) {
			size_t list_pos;
			size_t list_len;
			size_t list_end;

			if (ext_len < 2) {
				return STREAM_SNIFF_PARSE_ERROR;
			}

			list_len = read_u16_be(buf + ext_data);
			list_pos = ext_data + 2;
			list_end = list_pos + list_len;

			if (list_end != ext_end) {
				return STREAM_SNIFF_PARSE_ERROR;
			}

			while (list_pos + 3 <= list_end) {
				unsigned char name_type;
				uint16_t name_len;
				const unsigned char *name;

				name_type = buf[list_pos];
				name_len = read_u16_be(buf + list_pos + 1);
				list_pos += 3;

				if (list_pos + name_len > list_end) {
					return STREAM_SNIFF_PARSE_ERROR;
				}

				name = buf + list_pos;

				/*
				 * name_type 0 = host_name.
				 */
				if (name_type == 0) {
					if (name_len > STREAM_SNI_MAX) {
						return STREAM_SNIFF_NAME_TOO_LONG;
					}

					if (!valid_sni_name(name, name_len)) {
						return STREAM_SNIFF_PARSE_ERROR;
					}

					if (out_len < (size_t)name_len + 1) {
						return STREAM_SNIFF_NAME_TOO_LONG;
					}

					memcpy(out, name, name_len);
					out[name_len] = '\0';

					return STREAM_SNIFF_PARSED;
				}

				list_pos += name_len;
			}

			return STREAM_SNIFF_MISSING;
		}

		pos = ext_end;
	}

	if (pos != extensions_end) {
		return STREAM_SNIFF_PARSE_ERROR;
	}

	return STREAM_SNIFF_MISSING;
}

void stream_sniff_peek_client_input(
	struct stream_sniff *s,
	struct evbuffer *input)
{
	unsigned char buf[STREAM_SNIFF_MAX];
	size_t input_len;
	size_t copy_len;
	ssize_t n;
	bool need_more = false;
	enum stream_sniff_status status;

	if (s == NULL || input == NULL || s->done) {
		return;
	}

	input_len = evbuffer_get_length(input);
	if (input_len == 0) {
		return;
	}

	copy_len = input_len;
	if (copy_len > STREAM_SNIFF_MAX) {
		copy_len = STREAM_SNIFF_MAX;
	}

	n = evbuffer_copyout(input, buf, copy_len);
	if (n <= 0) {
		return;
	}

	s->observed = (size_t)n;

	status = parse_sni_from_client_hello(
		buf,
		(size_t)n,
		s->sni,
		sizeof(s->sni),
		&need_more
	);

	if (status == STREAM_SNIFF_NEED_MORE && need_more) {
		if (input_len >= STREAM_SNIFF_MAX) {
			s->status = STREAM_SNIFF_TRUNCATED;
			s->done = true;
			return;
		}

		s->status = STREAM_SNIFF_NEED_MORE;
		return;
	}

	s->status = status;

	switch (status) {
	case STREAM_SNIFF_PARSED:
	case STREAM_SNIFF_MISSING:
	case STREAM_SNIFF_NOT_TLS:
	case STREAM_SNIFF_TRUNCATED:
	case STREAM_SNIFF_NAME_TOO_LONG:
	case STREAM_SNIFF_PARSE_ERROR:
		s->done = true;
		break;

	case STREAM_SNIFF_NOT_OBSERVED:
	case STREAM_SNIFF_NEED_MORE:
	default:
		break;
	}

	if (s->done) {
		LOG_DEBUG("sni sniff",
			"sni", _LOGV(s->sni),
			"status", _LOGV(stream_sniff_status_str(s->status)),
			"observed", _LOGV(s->observed)
		);
	}
}

const char *stream_sniff_log_sni(const struct stream_sniff *s)
{
	if (s == NULL) {
		return "<not_observed>";
	}

	if (s->status == STREAM_SNIFF_PARSED && s->sni[0] != '\0') {
		return s->sni;
	}

	switch (s->status) {
	case STREAM_SNIFF_NOT_OBSERVED:
		return "<not_observed>";
	case STREAM_SNIFF_NEED_MORE:
		return "<need_more>";
	case STREAM_SNIFF_MISSING:
		return "<missing>";
	case STREAM_SNIFF_NOT_TLS:
		return "<not_tls>";
	case STREAM_SNIFF_TRUNCATED:
		return "<truncated>";
	case STREAM_SNIFF_NAME_TOO_LONG:
		return "<name_too_long>";
	case STREAM_SNIFF_PARSE_ERROR:
		return "<parse_error>";
	case STREAM_SNIFF_PARSED:
		return "<empty>";
	default:
		return "<unknown>";
	}
}
