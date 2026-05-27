#ifndef PROXY_PROTO_V2_H
#define PROXY_PROTO_V2_H

#define PROXY_V2_SIG "\r\n\r\n\0\r\nQUIT\n"

#define PP2_VERSION_CMD_PROXY 0x21

#define PP2_FAM_INET		  0x10
#define PP2_FAM_INET6		 0x20
#define PP2_FAM_UNIX		  0x30

#define PP2_TRANS_STREAM	  0x01
#define PP2_TRANS_DGRAM	   0x02

struct bufferevent;

int proxy_v2_build(
	unsigned char *buf,
	size_t buf_len,
	const struct sockaddr *src,
	socklen_t src_len,
	const struct sockaddr *dst,
	socklen_t dst_len,
	int sock_type,
	size_t *out_len
);

int proxy_v2_write_bufferevent(
	struct bufferevent *bev,
	const struct sockaddr *src,
	socklen_t src_len,
	const struct sockaddr *dst,
	socklen_t dst_len,
	int sock_type
);

#endif
