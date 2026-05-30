#include "compat_cpu.h"

#include <limits.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

int compat_cpu_count(void)
{
#ifdef _WIN32
	SYSTEM_INFO si;

	GetSystemInfo(&si);

	if (si.dwNumberOfProcessors == 0 ||
		si.dwNumberOfProcessors > INT_MAX) {
		return 1;
	}

	return (int)si.dwNumberOfProcessors;
#else
	long n = sysconf(_SC_NPROCESSORS_ONLN);

	if (n < 1 || n > INT_MAX) {
		return 1;
	}

	return (int)n;
#endif
}
