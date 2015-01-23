/*
 * deflate_compress.h
 *
 * This file has no copyright assigned and is placed in the Public Domain.
 */

#pragma once

/* 'struct deflate_compressor' is private to deflate_compress.c, but zlib header
 * generation needs to be able to query the compression level.  */

struct deflate_compressor;

extern unsigned int
deflate_get_compression_level(struct deflate_compressor *c);
