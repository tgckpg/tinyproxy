#ifndef COMPAT_FILE_H
#define COMPAT_FILE_H

#ifdef _WIN32
#include <direct.h>
#include <errno.h>

static inline int compat_mkdir(const char *path, int mode)
{
	(void)mode;
	return _mkdir(path);
}

#else
#include <sys/stat.h>
#include <sys/types.h>

static inline int compat_mkdir(const char *path, mode_t mode)
{
	return mkdir(path, mode);
}

#endif

#endif
