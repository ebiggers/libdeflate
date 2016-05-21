/*
 * aligned_malloc.c - aligned memory allocation
 *
 * Written in 2014-2015 by Eric Biggers <ebiggers3@gmail.com>
 *
 * To the extent possible under law, the author(s) have dedicated all copyright
 * and related and neighboring rights to this software to the public domain
 * worldwide. This software is distributed without any warranty.
 *
 * You should have received a copy of the CC0 Public Domain Dedication along
 * with this software. If not, see
 * <http://creativecommons.org/publicdomain/zero/1.0/>.
 */

/*
 * This file provides portable aligned memory allocation functions that only
 * use malloc() and free().  This avoids portability problems with
 * posix_memalign(), aligned_alloc(), etc.
 */

#include <stdlib.h>

#include "aligned_malloc.h"

void *
aligned_malloc(size_t alignment, size_t size)
{
	const uintptr_t mask = alignment - 1;
	char *ptr = NULL;
	char *raw_ptr;

	raw_ptr = malloc(mask + sizeof(size_t) + size);
	if (raw_ptr) {
		ptr = (char *)raw_ptr + sizeof(size_t);
		ptr = (void *)(((uintptr_t)ptr + mask) & ~mask);
		*((size_t *)ptr - 1) = ptr - raw_ptr;
	}
	return ptr;
}

void
aligned_free(void *ptr)
{
	if (ptr)
		free((char *)ptr - *((size_t *)ptr - 1));
}
