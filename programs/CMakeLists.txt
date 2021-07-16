
# For examples of checking symbols existence, see
# https://go.googlesource.com/gollvm/+/master/cmake/modules/ConfigSetup.cmake
#
include(CheckSymbolExists)

check_symbol_exists(clock_gettime "time.h" HAVE_CLOCK_GETTIME)
check_symbol_exists(futimens "fcntl.h;sys/stat.h" HAVE_FUTIMENS)
check_symbol_exists(futimes "sys/time.h" HAVE_FUTIMES)
check_symbol_exists(posix_fadvise "fcntl.h" HAVE_POSIX_FADVISE)
check_symbol_exists(posix_madvise "sys/mman.h" HAVE_POSIX_MADVISE)

check_c_source_compiles("#include <sys/types.h>
			 #include <sys/stat.h>
                         int main() { struct stat st; (void) st.atim; }"
                         HAVE_STAT_NANOSECOND_PRECISION)

check_c_source_compiles("#include <sys/types.h>
                        #include <sys/stat.h>
                        int main() { struct stat st; (void) st.st_atimespec; }"
                        HAVE_STAT_NANOSECOND_PRECISION_MACOS)

if (${HAVE_STAT_NANOSECOND_PRECISION_MACOS})
  set(HAVE_STAT_NANOSECOND_PRECISION 1)
  set(st_atim st_atimensec)
  set(st_mtim st_mtimensec)
  set(st_ctim st_ctimensec)
endif()

configure_file(${PROJECT_SOURCE_DIR}/programs/config.h.in 
               ${PROJECT_SOURCE_DIR}/programs/config.h)

set(TEST_PROGRAMS_DEFINITIONS _POSIX_C_SOURCE=200809L
                              _FILE_OFFSET_BITS=64
                              HAVE_CONFIG_H)

if (DEFLATE_TEST_SUPPORT__DO_NOT_USE)
  list(APPEND TEST_PROGRAMS_DEFINITIONS TEST_SUPPORT__DO_NOT_USE)
endif()

if (BUILD_SHARED_LIBS)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Small library for common code
add_library(programs)
target_sources(programs PRIVATE prog_util.c test_util.c tgetopt.c)
target_compile_definitions(programs PRIVATE ${TEST_PROGRAMS_DEFINITIONS})
target_link_libraries(programs deflate)

# cli
set(PROGRAM_TARGET_NAMES
  gzip
  benchmark
  checksum
)
foreach(PROGRAM_TARGET_NAME ${PROGRAM_TARGET_NAMES})
  add_executable(${PROGRAM_TARGET_NAME} ${PROGRAM_TARGET_NAME}.c)
  target_link_libraries(${PROGRAM_TARGET_NAME} programs deflate z)
  target_compile_definitions(${PROGRAM_TARGET_NAME} PRIVATE ${TEST_PROGRAMS_DEFINITIONS})
endforeach()

# tests
set(TEST_TARGET_NAMES
  test_checksums
  test_custom_malloc
  test_incomplete_codes
  test_litrunlen_overflow
  test_slow_decompression
  test_trailing_bytes
)
foreach(TEST_TARGET_NAME ${TEST_TARGET_NAMES})
  add_executable(${TEST_TARGET_NAME} ${TEST_TARGET_NAME}.c)
  target_link_libraries(${TEST_TARGET_NAME} programs deflate z)
  target_compile_definitions(${TEST_TARGET_NAME} PRIVATE ${TEST_PROGRAMS_DEFINITIONS})

  add_test(NAME ${TEST_TARGET_NAME} COMMAND ${TEST_TARGET_NAME})
endforeach()
