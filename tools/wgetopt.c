/*
 * wide-character getopt for Windows
 *
 * Public domain
 */

#include <stdio.h>
#include <wchar.h>

#include "wgetopt.h"

wchar_t *woptarg;		/* Global argument pointer */
int woptind = 0;		/* Global argv index */

static wchar_t *scan = NULL;	/* Private scan pointer */

int wgetopt(int argc, wchar_t * const argv[], const wchar_t *optstring)
{
	wchar_t c;
	wchar_t *place;

	woptarg = NULL;

	if (!scan || *scan == '\0') {
		if (woptind == 0)
			woptind++;
		if (woptind >= argc || argv[woptind][0] != '-'
		    || argv[woptind][1] == '\0')
			return EOF;
		if (argv[woptind][1] == '-' && argv[woptind][2] == '\0') {
			woptind++;
			return EOF;
		}
		scan = argv[woptind] + 1;
		woptind++;
	}

	c = *scan++;
	place = wcschr(optstring, c);

	if (!place || c == ':') {
		fprintf(stderr, "%ls: unknown option -%lc\n", argv[0], c);
		return '?';
	}

	place++;
	if (*place == ':') {
		if (*scan != '\0') {
			woptarg = scan;
			scan = NULL;
		} else if (woptind < argc) {
			woptarg = argv[woptind];
			woptind++;
		} else {
			fprintf(stderr, "%ls: option requires argument -%lc\n",
				argv[0], c);
			return ':';
		}
	}

	return c;
}
