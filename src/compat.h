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

#endif
