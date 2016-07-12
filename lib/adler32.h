/*
 * adler32.h - Adler-32 checksum algorithm
 */

#ifndef _LIB_ADLER32_H
#define _LIB_ADLER32_H

#include "lib_common.h"

extern u32 adler32_zlib(const void *buffer, size_t size);

#endif /* _LIB_ADLER32_H */
