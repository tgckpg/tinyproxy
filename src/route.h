#ifndef ROUTE_H
#define ROUTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "endpoint.h"

#define ROUTE_HOST_MAX 256
#define ROUTE_DEFAULT_IDLE_TIMEOUT_SEC 60
#define ROUTE_DEFAULT_CONNECT_TIMEOUT_SEC 5

#define BEV_READ_HIGH_WATER (256 * 1024)
#define BEV_WRITE_RESUME_WATER (128 * 1024)

#define CLOSE_WAIT_TIMEOUT_SEC 2

#define UDP_MAX_PACKET 65535

enum broadcast_reply_mode {
	BROADCAST_REPLY_OFF = 0,
	BROADCAST_REPLY_LISTEN,
	BROADCAST_REPLY_UPSTREAM,
};

struct route_options {
	bool proxy_v2;
	bool keep_alive;
	bool sni_sniff;

	int idle_timeout_sec;
	int connect_timeout_sec;
	enum broadcast_reply_mode broadcast_reply;
};

struct route {
	struct endpoint listen;
	struct endpoint upstream;

	struct route_options opts;

	unsigned int line_no;
};

void route_options_str(const struct route_options *opts, char *buf, size_t buflen);

#endif
