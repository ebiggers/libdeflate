/*
 * crc32.h - CRC-32 checksum algorithm for the gzip format
 */

#ifndef _LIB_CRC32_H
#define _LIB_CRC32_H

#include "lib_common.h"

extern u32 crc32_gzip(const void *buffer, size_t size);

#endif /* _LIB_CRC32_H */
