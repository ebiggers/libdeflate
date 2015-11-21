/*
 * crc32.h - CRC-32 checksum algorithm for the gzip format
 */

#pragma once

#include "util.h"

extern u32
crc32_gzip(const void *buffer, size_t size);
