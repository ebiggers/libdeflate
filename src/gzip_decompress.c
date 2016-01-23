/*
 * gzip_decompress.c - decompress with a gzip wrapper
 *
 * Author:	Eric Biggers
 * Year:	2014, 2015
 *
 * The author dedicates this file to the public domain.
 * You can do whatever you want with this file.
 */

#include "libdeflate.h"

#include "crc32.h"
#include "gzip_constants.h"
#include "unaligned.h"

LIBEXPORT bool
gzip_decompress(struct deflate_decompressor *d,
		const void *in, size_t in_nbytes,
		void *out, size_t out_nbytes_avail,
		size_t *actual_out_nbytes_ret)
{
	const u8 *in_next = in;
	const u8 * const in_end = in_next + in_nbytes;
	u8 flg;
	size_t actual_out_nbytes;

	if (in_nbytes < GZIP_MIN_OVERHEAD)
		return false;

	/* ID1 */
	if (*in_next++ != GZIP_ID1)
		return false;
	/* ID2 */
	if (*in_next++ != GZIP_ID2)
		return false;
	/* CM */
	if (*in_next++ != GZIP_CM_DEFLATE)
		return false;
	flg = *in_next++;
	/* MTIME */
	in_next += 4;
	/* XFL */
	in_next += 1;
	/* OS */
	in_next += 1;

	if (flg & GZIP_FRESERVED)
		return false;

	/* Extra field */
	if (flg & GZIP_FEXTRA) {
		u16 xlen = get_unaligned_le16(in_next);
		in_next += 2;

		if (in_end - in_next < (u32)xlen + GZIP_FOOTER_SIZE)
			return false;

		in_next += xlen;
	}

	/* Original file name (zero terminated) */
	if (flg & GZIP_FNAME) {
		while (*in_next++ != 0 && in_next != in_end)
			;
		if (in_end - in_next < GZIP_FOOTER_SIZE)
			return false;
	}

	/* File comment (zero terminated) */
	if (flg & GZIP_FCOMMENT) {
		while (*in_next++ != 0 && in_next != in_end)
			;
		if (in_end - in_next < GZIP_FOOTER_SIZE)
			return false;
	}

	/* CRC16 for gzip header */
	if (flg & GZIP_FHCRC) {
		in_next += 2;
		if (in_end - in_next < GZIP_FOOTER_SIZE)
			return false;
	}

	/* Compressed data  */
	if (!deflate_decompress(d, in_next, in_end - GZIP_FOOTER_SIZE - in_next,
				out, out_nbytes_avail, actual_out_nbytes_ret))
		return false;

	if (actual_out_nbytes_ret)
		actual_out_nbytes = *actual_out_nbytes_ret;
	else
		actual_out_nbytes = out_nbytes_avail;

	in_next = in_end - GZIP_FOOTER_SIZE;

	/* CRC32 */
	if (crc32_gzip(out, actual_out_nbytes) != get_unaligned_le32(in_next))
		return false;
	in_next += 4;

	/* ISIZE */
	if (actual_out_nbytes != get_unaligned_le32(in_next))
		return false;

	return true;
}
