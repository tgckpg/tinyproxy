#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>

struct endpoint;

enum log_value_type {
	LOG_VALUE_STR = 1,
	LOG_VALUE_INT,
	LOG_VALUE_UINT,
	LOG_VALUE_LONG,
	LOG_VALUE_ULONG,
	LOG_VALUE_LLONG,
	LOG_VALUE_ULLONG,
	LOG_VALUE_ENDPOINT,
};

struct log_value {
	enum log_value_type type;
	union {
		const char *s;
		int i;
		unsigned int u;
		long l;
		unsigned long ul;
		long long ll;
		unsigned long long ull;
		const struct endpoint *endpoint;
	} v;
};

void log_at(char sev, const char *file, int line, const char *msg, ...);

static inline struct log_value log_value_str(const char *v)
{
	return (struct log_value){ LOG_VALUE_STR, { .s = v } };
}

static inline struct log_value log_value_int(int v)
{
	return (struct log_value){ LOG_VALUE_INT, { .i = v } };
}

static inline struct log_value log_value_uint(unsigned int v)
{
	return (struct log_value){ LOG_VALUE_UINT, { .u = v } };
}

static inline struct log_value log_value_long(long v)
{
	return (struct log_value){ LOG_VALUE_LONG, { .l = v } };
}

static inline struct log_value log_value_ulong(unsigned long v)
{
	return (struct log_value){ LOG_VALUE_ULONG, { .ul = v } };
}

static inline struct log_value log_value_llong(long long v)
{
	return (struct log_value){ LOG_VALUE_LLONG, { .ll = v } };
}

static inline struct log_value log_value_ullong(unsigned long long v)
{
	return (struct log_value){ LOG_VALUE_ULLONG, { .ull = v } };
}

static inline struct log_value log_value_endpoint(const struct endpoint *v)
{
	return (struct log_value){ LOG_VALUE_ENDPOINT, { .endpoint = v } };
}

#define _LOGV(v) \
	_Generic((v), \
		char *: log_value_str, \
		const char *: log_value_str, \
		signed char: log_value_int, \
		unsigned char: log_value_uint, \
		short: log_value_int, \
		unsigned short: log_value_uint, \
		int: log_value_int, \
		unsigned int: log_value_uint, \
		long: log_value_long, \
		unsigned long: log_value_ulong, \
		long long: log_value_llong, \
		unsigned long long: log_value_ullong \
	)(v)

#define _LOGV_ENDPOINT(v) log_value_endpoint(v)

/*
 * Structured fields must be passed as:
 *
 *   LOG_INFO("message", "key", _LOGV(value), ...);
 *
 * Do not pass raw values:
 *
 *   LOG_INFO("message", "key", value); // wrong
 */

#define LOG_INFO(msg, ...)  log_at('I', __FILE__, __LINE__, (msg), ##__VA_ARGS__, NULL)
#define LOG_WARN(msg, ...)  log_at('W', __FILE__, __LINE__, (msg), ##__VA_ARGS__, NULL)
#define LOG_ERROR(msg, ...) log_at('E', __FILE__, __LINE__, (msg), ##__VA_ARGS__, NULL)

#endif
