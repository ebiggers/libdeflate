cmake_minimum_required(VERSION 3.5.1)
include(CheckCCompilerFlag)

project(deflate C ASM)

set(CMAKE_C_STANDARD 99)

#
# Options
#
# Generic option
option(BUILD_SHARED_LIBS "Build shared libraries (.dll/.so) instead of static ones (.lib/.a)" OFF)

# libdeflate options
option(DEFLATE_DECOMPRESSION_ONLY "Omit all compression code, building a decompression-only library" OFF)

option(DEFLATE_DISABLE_ZLIB "disable support for the zlib wrapper format" OFF)

option(DEFLATE_DISABLE_GZIP "disable support for the gzip wrapper format" OFF)

option(DEFLATE_FREESTANDING "build a freestanding library, i.e. a library that doesn't 
                             link to any libc functions like malloc(), free(),
                             and memcpy(). All users will need to call
                             libdeflate_set_memory_allocator()." OFF)

option(DEFLATE_BUILD_TEST_PROGRAMS "Build test programs" OFF)
option(DEFLATE_TEST_SUPPORT__DO_NOT_USE "Don't use this option except for testing; it isn't a stable interface." OFF)

# Source code
set(LIBDEFLATE_SOURCES
  lib/deflate_decompress.c
  lib/utils.c
  lib/x86/cpu_features.c
  lib/zlib_compress.c
  lib/arm/cpu_features.c
)

if (NOT DEFLATE_DECOMPRESSION_ONLY)
  list(APPEND LIBDEFLATE_SOURCES lib/deflate_compress.c)
endif()

if (NOT DEFLATE_DISABLE_ZLIB)
  list(APPEND LIBDEFLATE_SOURCES lib/adler32.c lib/zlib_decompress.c)
  if (NOT DEFLATE_DECOMPRESSION_ONLY)
    list(APPEND LIBDEFLATE_SOURCES lib/zlib_compress.c)
  endif()
endif()

if (NOT DEFLATE_DISABLE_GZIP)
  list(APPEND LIBDEFLATE_SOURCES lib/crc32.c lib/gzip_decompress.c)
  if (NOT DEFLATE_DECOMPRESSION_ONLY)
    list(APPEND LIBDEFLATE_SOURCES lib/gzip_compress.c)
  endif()
endif()

set(LIBDEFLATE_HEADERS
  common/compiler_msc.h
  common/compiler_gcc.h
  common/common_defs.h
  lib/bt_matchfinder.h
  lib/matchfinder_common.h
  lib/unaligned.h
  lib/gzip_constants.h
  lib/zlib_constants.h
  lib/crc32_vec_template.h
  lib/decompress_template.h
  lib/deflate_compress.h
  lib/crc32_table.h
  lib/x86/decompress_impl.h
  lib/x86/crc32_impl.h
  lib/x86/cpu_features.h
  lib/x86/adler32_impl.h
  lib/x86/matchfinder_impl.h
  lib/x86/crc32_pclmul_template.h
  lib/lib_common.h
  lib/hc_matchfinder.h
  lib/cpu_features_common.h
  lib/arm/crc32_impl.h
  lib/arm/cpu_features.h
  lib/arm/adler32_impl.h
  lib/arm/matchfinder_impl.h
  lib/deflate_constants.h
  lib/adler32_vec_template.h
)

add_library(deflate)
add_library(deflate::deflate ALIAS deflate)

set_target_properties(deflate PROPERTIES VERSION 0.0.0 SOVERSION 0)

target_sources(deflate PRIVATE ${LIBDEFLATE_SOURCES}
                               ${LIBDEFLATE_HEADERS})

# Compile options
check_c_compiler_flag(-Wall DEFLATE_LINT_WALL)
check_c_compiler_flag(-Wundef DEFLATE_LINT_WUNDEF)
check_c_compiler_flag(-Wpedantic DEFLATE_LINT_PEDANTIC)
check_c_compiler_flag(-Wdeclaration-after-statement DEFLATE_LINT_DECLARATION_AFTER_STATEMENT)
check_c_compiler_flag(-Wmissing-prototypes DEFLATE_LINT_MISSING_PROTOTYPES)
check_c_compiler_flag(-Wstrict-prototypes DEFLATE_LINT_STRICT_PROTOTYPES)
check_c_compiler_flag(-Wvla DEFLATE_LINT_VLA)
check_c_compiler_flag(-Wimplicit-fallthrough DEFLATE_LINT_IMPLICIT_FALLTHROUGH)

set(lint-all $<$<BOOL:${DEFLATE_LINT_WALL}>:-Wall>)
set(lint-undef $<$<BOOL:${DEFLATE_LINT_UNDEF}>:-Wundef>)
set(lint-pedantic $<$<BOOL:${DEFLATE_LINT_PEDANTIC}>:-Wpedantic>)
set(lint-declaration-after-statement $<$<BOOL:${DEFLATE_LINT_DECLARATION_AFTER_STATEMENT}>:-Wdeclaration-after-statement>)
set(lint-missing-prototypes $<$<BOOL:${DEFLATE_LINT_MISSING_PROTOTYPES}>:-Wmissing-prototypes>)
set(lint-strict-prototypes $<$<BOOL:${DEFLATE_LINT_STRICT_PROTOTYPES}>:-Wstrict-prototypes>)
set(lint-vla $<$<BOOL:${DEFLATE_LINT_VLA}>:-Wvla>)
set(lint-implicit-fallthrough $<$<BOOL:${DEFLATE_LINT_IMPLICIT_FALLTHROUGH}>:-Wimplicit-fallthrough>)

target_compile_options(deflate PRIVATE ${lint-all})
target_compile_options(deflate PRIVATE ${lint-undef})
target_compile_options(deflate PRIVATE ${lint-pedantic})
target_compile_options(deflate PRIVATE ${lint-declaration-after-statement})
target_compile_options(deflate PRIVATE ${lint-missing-prototypes})
target_compile_options(deflate PRIVATE ${lint-strict-prototypes})
target_compile_options(deflate PRIVATE ${lint-vla})
target_compile_options(deflate PRIVATE ${lint-implicit-fallthrough})

# The CFLAGS environment variable is read and handled automatically by cmake

if (BUILD_SHARED_LIBS)
  target_compile_definitions(deflate PRIVATE _ANSI_SOURCE)
  set_target_properties(deflate PROPERTIES C_VISIBILITY_PRESET hidden)

  if (WIN32)
    target_compile_definitions(deflate PUBLIC LIBDEFLATE_DLL)
  endif()
endif()

if (FREESTANDING)
  target_compile_definitions(deflate PRIVATE FREESTANDING)
  target_compile_options(deflate PRIVATE -ffreestanding -nostdlib)
endif()

if (DEFLATE_TEST_SUPPORT__DO_NOT_USE)
  target_compile_definitions(deflate PRIVATE TEST_SUPPORT__DO_NOT_USE)
endif()

target_include_directories(deflate PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:include>
)

set_target_properties(deflate PROPERTIES PUBLIC_HEADER "libdeflate.h")

install(TARGETS deflate
        EXPORT deflate
        ARCHIVE DESTINATION lib
        PUBLIC_HEADER DESTINATION include/
)

# This is required to work with FetchContent
install(EXPORT deflate
        FILE deflate-config.cmake
        NAMESPACE deflate::
        DESTINATION lib/cmake/deflate)

if (DEFLATE_BUILD_TEST_PROGRAMS)
  enable_testing()
  add_subdirectory(programs)
endif()
