#ifndef KLOG_H
#define KLOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "compat.h"

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

void klog_set_worker_id(int id);

int klog_init(void);
void klog_free(void);
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

#ifndef TINYPROXY_SOCKADDR_FORMAT_BUFSIZE
#define TINYPROXY_SOCKADDR_FORMAT_BUFSIZE 128
#endif

static inline const char *sockaddr_format(
	const struct sockaddr *addr,
	socklen_t addr_len,
	char *buf,
	size_t buf_len)
{
	if (!buf || buf_len == 0) {
		return "";
	}

	buf[0] = '\0';

	if (!addr || addr_len == 0) {
		snprintf(buf, buf_len, "<null>");
		return buf;
	}

	switch (addr->sa_family) {
	case AF_INET: {
		const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
		char ip[INET_ADDRSTRLEN];

		if (addr_len < sizeof(*in)) {
			snprintf(buf, buf_len, "inet:<short:%u>", (unsigned)addr_len);
			return buf;
		}

		if (!inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip))) {
			snprintf(buf, buf_len, "inet:<invalid>:%u",
				(unsigned)ntohs(in->sin_port));
			return buf;
		}

		snprintf(buf, buf_len, "%s:%u",
			ip,
			(unsigned)ntohs(in->sin_port));
		return buf;
	}

#ifdef AF_INET6
	case AF_INET6: {
		const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)addr;
		char ip[INET6_ADDRSTRLEN];

		if (addr_len < sizeof(*in6)) {
			snprintf(buf, buf_len, "inet6:<short:%u>", (unsigned)addr_len);
			return buf;
		}

		if (!inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof(ip))) {
			snprintf(buf, buf_len, "inet6:<invalid>:%u",
				(unsigned)ntohs(in6->sin6_port));
			return buf;
		}

		snprintf(buf, buf_len, "[%s]:%u",
			ip,
			(unsigned)ntohs(in6->sin6_port));
		return buf;
	}
#endif

#ifndef _WIN32
	case AF_UNIX: {
		const struct sockaddr_un *un = (const struct sockaddr_un *)addr;

		if (addr_len < sizeof(un->sun_family)) {
			snprintf(buf, buf_len, "unix:<short:%u>", (unsigned)addr_len);
			return buf;
		}

		if (un->sun_path[0] == '\0') {
			snprintf(buf, buf_len, "unix:<abstract-or-empty>");
			return buf;
		}

		snprintf(buf, buf_len, "unix:%s", un->sun_path);
		return buf;
	}
#endif

	default:
		snprintf(buf, buf_len, "family:%d len:%u",
			(int)addr->sa_family,
			(unsigned)addr_len);
		return buf;
	}
}

#define _LOGV_SOCKADDR(addr, addr_len, buf, buf_len) \
	_LOGV(sockaddr_format((const struct sockaddr *)(addr), (addr_len), (buf), (buf_len)))

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

#ifdef TINYPROXY_DEBUG
#define LOG_DEBUG(msg, ...) \
	LOG_INFO((msg), __VA_ARGS__)
#else
#define LOG_DEBUG(msg, ...) \
	do { } while (0)
#endif

#endif
