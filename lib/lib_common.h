/*
 * lib_common.h - internal header included by all library code
 */

#ifndef LIB_LIB_COMMON_H
#define LIB_LIB_COMMON_H

#ifdef LIBDEFLATE_H
#  error "lib_common.h must always be included before libdeflate.h"
   /* because BUILDING_LIBDEFLATE must be set first */
#endif

#define BUILDING_LIBDEFLATE

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
#define aligned_malloc			SYM_FIXUP(aligned_malloc)
#define aligned_free			SYM_FIXUP(aligned_free)
#define deflate_get_compression_level	SYM_FIXUP(deflate_get_compression_level)
#define _cpu_features			SYM_FIXUP(_cpu_features)
#define setup_cpu_features		SYM_FIXUP(setup_cpu_features)

#endif /* LIB_LIB_COMMON_H */
