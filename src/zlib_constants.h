/*
 * zlib_constants.h
 *
 * Constants for the zlib wrapper format.
 */

#pragma once

#define ZLIB_MIN_HEADER_SIZE	2
#define ZLIB_FOOTER_SIZE	4
#define ZLIB_MIN_OVERHEAD	(ZLIB_MIN_HEADER_SIZE + ZLIB_FOOTER_SIZE)

#define ZLIB_CM_DEFLATE		8

#define ZLIB_CINFO_32K_WINDOW	7

#define ZLIB_FASTEST_COMPRESSION	0
#define ZLIB_FAST_COMPRESSION		1
#define ZLIB_DEFAULT_COMPRESSION	2
#define ZLIB_SLOWEST_COMPRESSION	3
