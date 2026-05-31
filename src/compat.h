#ifndef COMPAT_H
#define COMPAT_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <process.h>
#include <direct.h>
#include <errno.h>

#else

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <unistd.h>


#endif

#ifdef _WIN32
#define compat_unlink(path) _unlink(path)
#else
#define compat_unlink(path) unlink(path)
#endif

static inline int socket_err_is_retriable(int err)
{
#ifdef _WIN32
	return err == WSAEWOULDBLOCK || err == WSAEINTR;
#else
	return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
#endif
}

static inline int compat_getpid(void)
{
#ifdef _WIN32
	return _getpid();
#else
	return (int)getpid();
#endif
}

static inline int compat_mkdir(const char *path, int mode)
{
#ifdef _WIN32
	(void)mode;
	return _mkdir(path);
#else
	return mkdir(path, mode);
#endif
}

static inline uint16_t compat_raw_ip_len(uint16_t len)
{
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
	return len;
#else
	return htons(len);
#endif
}

static inline uint16_t compat_raw_ip_off(uint16_t off)
{
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
	return off;
#else
	return htons(off);
#endif
}

#if defined(AF_INET) && \
	defined(SOCK_RAW) && \
	defined(IPPROTO_RAW) && \
	defined(IPPROTO_IP) && \
	defined(IP_HDRINCL)
#define COMPAT_HAS_IPV4_RAW 1
#else
#define COMPAT_HAS_IPV4_RAW 0
#endif

#if !COMPAT_HAS_IPV4_RAW
#if defined(_MSC_VER)
#pragma message("info: IPv4 raw socket support not available; raw datagram features disabled")
#elif defined(__clang__) || defined(__GNUC__)
#pragma message "info: IPv4 raw socket support not available; raw datagram features disabled"
#endif
#endif

#endif
