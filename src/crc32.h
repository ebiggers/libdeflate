/*
 * crc32.h
 *
 * CRC-32 checksum algorithm.
 */

#pragma once

#include "types.h"

extern u32
crc32(const u8 *buffer, size_t size);
