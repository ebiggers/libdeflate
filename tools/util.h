#define _FILE_OFFSET_BITS 64
#undef _ANSI_SOURCE
#define _POSIX_C_SOURCE 199309L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#  include <wchar.h>
#  include <windows.h>
#  include "wgetopt.h"
#else
#  include <sys/mman.h>
#  include <sys/time.h>
#  include <unistd.h>
#  include <getopt.h>
#endif

#include <libdeflate.h>

#ifdef _WIN32
#  define 	main		wmain
#  define	tchar		wchar_t
#  define	_T(text)	L##text
#  define	T(text)		_T(text)
#  define	TS		"ls"
#  define	tgetopt		wgetopt
#  define	toptarg		woptarg
#  define	toptind		woptind
#  define	tstrcmp		wcscmp
#  define	tstrlen		wcslen
#  define	tmemcpy		wmemcpy
#  define	tstrrchr	wcsrchr
#  define 	tstrtol		wcstol
#  define 	tstrtoul	wcstoul
#  define 	topen		_wopen
#  define 	tunlink		_wunlink
#  ifdef _MSC_VER
#    define 	fstat		_fstat64
#    define 	stat		_stat64
#    define	S_ISREG(m)      (((m) & S_IFMT) == S_IFREG)
#  endif
#  define 	tstat		_wstati64
#else
#  define 	main		main
#  define 	tchar		char
#  define 	T(text)		text
#  define	TS		"s"
#  define	tgetopt		getopt
#  define	toptarg		optarg
#  define	toptind		optind
#  define 	tstrcmp		strcmp
#  define	tstrlen		strlen
#  define	tmemcpy		memcpy
#  define 	tstrrchr	strrchr
#  define 	tstrtol		strtol
#  define 	tstrtoul	strtoul
#  define 	topen		open
#  define 	tunlink		unlink
#  define 	tstat		stat
#endif

#ifndef O_BINARY
#  define O_BINARY 0
#endif

#ifndef STDIN_FILENO
#  define STDIN_FILENO 0
#endif

static void
fatal_error(const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	fprintf(stderr, "ERROR: ");
	vfprintf(stderr, fmt, va);
	fprintf(stderr, "\n");
	va_end(va);

	exit(1);
}

#define ASSERT(expr, fmt, ...)				\
{							\
	if (!(expr))					\
		fatal_error((fmt), ## __VA_ARGS__);	\
}

static inline const tchar *
basename(const tchar *argv0)
{
	const tchar *p = tstrrchr(argv0, '/');
#ifdef _WIN32
	const tchar *p2 = tstrrchr(argv0, '\\');
	if (p2 && (!p || p2 > p))
		p = p2;
#endif
	if (p)
		return p + 1;
	return argv0;
}

static inline void
set_binary_mode(int fd)
{
#ifdef _WIN32
	_setmode(fd, O_BINARY);
#endif
}

static inline uint64_t
current_time(void)
{
#ifdef _WIN32
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	return 100 * (((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)1000000000 * ts.tv_sec) + ts.tv_nsec;
#endif
}
