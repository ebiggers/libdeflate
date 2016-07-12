/*
 * lib_common.h - internal header included by all library code
 */

#ifndef _LIB_LIB_COMMON_H
#define _LIB_LIB_COMMON_H

#include "common_defs.h"

/*
 * Prefix with "_libdeflate_" all global symbols which are not part of the API.
 * This avoids exposing overly generic names when libdeflate is built as a
 * static library.
 *
 * Note that the chosen prefix is not really important and can be changed
 * without breaking library users.  It was just chosen so that the resulting
 * symbol names are unlikely to conflict with those from any other software.
 * Also note that this fixup has no useful effect when libdeflate is built as a
 * shared library, since these symbols are not exported.
 */
#define SYM_FIXUP(sym)			_libdeflate_##sym
#define adler32				SYM_FIXUP(adler32)
#define aligned_malloc			SYM_FIXUP(aligned_malloc)
#define aligned_free			SYM_FIXUP(aligned_free)
#define crc32_gzip			SYM_FIXUP(crc32_gzip)
#define deflate_get_compression_level	SYM_FIXUP(deflate_get_compression_level)
#define _x86_cpu_features		SYM_FIXUP(_x86_cpu_features)
#define x86_setup_cpu_features		SYM_FIXUP(x86_setup_cpu_features)

#endif /* _LIB_LIB_COMMON_H */
