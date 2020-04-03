/*
 * utils.c - utility functions for libdeflate
 *
 * Copyright 2016 Eric Biggers
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdlib.h>

#include "lib_common.h"

#include "libdeflate.h"

static void *(*libdeflate_malloc_func)(size_t) = malloc;
static void (*libdeflate_free_func)(void *) = free;

void *
libdeflate_malloc(size_t size)
{
	return (*libdeflate_malloc_func)(size);
}

void
libdeflate_free(void *ptr)
{
	(*libdeflate_free_func)(ptr);
}

void *
libdeflate_aligned_malloc(size_t alignment, size_t size)
{
	void *ptr = libdeflate_malloc(sizeof(void *) + alignment - 1 + size);
	if (ptr) {
		void *orig_ptr = ptr;
		ptr = (void *)ALIGN((uintptr_t)ptr + sizeof(void *), alignment);
		((void **)ptr)[-1] = orig_ptr;
	}
	return ptr;
}

void
libdeflate_aligned_free(void *ptr)
{
	if (ptr)
		libdeflate_free(((void **)ptr)[-1]);
}

LIBDEFLATEEXPORT void LIBDEFLATEAPI
libdeflate_set_memory_allocator(void *(*malloc_func)(size_t),
				void (*free_func)(void *))
{
	libdeflate_malloc_func = malloc_func;
	libdeflate_free_func = free_func;
}
