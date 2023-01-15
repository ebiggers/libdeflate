include(CheckSymbolExists)

# Check for the availability of OS functionality and generate the config.h file.
#
# Keep CMAKE_REQUIRED_DEFINITIONS in sync with what prog_util.h does.
if(LINUX)
    set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L)
elseif(APPLE)
    set(CMAKE_REQUIRED_DEFINITIONS -D_DARWIN_C_SOURCE -U_POSIX_C_SOURCE)
else()
    set(CMAKE_REQUIRED_DEFINITIONS -U_POSIX_C_SOURCE)
endif()
check_symbol_exists(clock_gettime "time.h" HAVE_CLOCK_GETTIME)
check_symbol_exists(futimens "fcntl.h;sys/stat.h" HAVE_FUTIMENS)
check_symbol_exists(posix_fadvise "fcntl.h" HAVE_POSIX_FADVISE)
check_symbol_exists(posix_madvise "sys/mman.h" HAVE_POSIX_MADVISE)
check_c_source_compiles("#include <sys/types.h>
                         #include <sys/stat.h>
                         int main() { struct stat st; (void)st.st_atim; }"
                         HAVE_STAT_NANOSECOND_PRECISION)
configure_file(config.h.in config.h)

# Build a utility library for the programs.  This library is not installed.
add_library(libdeflate_prog_utils STATIC prog_util.c tgetopt.c ../common_defs.h)
set_target_properties(libdeflate_prog_utils PROPERTIES
                      OUTPUT_NAME deflate_prog_utils)
if(LIBDEFLATE_USE_SHARED_LIB)
    target_link_libraries(libdeflate_prog_utils PUBLIC libdeflate_shared)
else()
    target_link_libraries(libdeflate_prog_utils PUBLIC libdeflate_static)
endif()
target_include_directories(libdeflate_prog_utils PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(libdeflate_prog_utils PUBLIC HAVE_CONFIG_H)
if(WIN32)
    if(MINGW)
        target_compile_options(libdeflate_prog_utils PUBLIC -municode)
        target_link_libraries(libdeflate_prog_utils PUBLIC -municode)
    else()
        target_compile_definitions(libdeflate_prog_utils PUBLIC UNICODE _UNICODE)
    endif()
endif()

# Build and install libdeflate-gzip and its alias libdeflate-gunzip.
if(LIBDEFLATE_BUILD_GZIP)
    add_executable(libdeflate-gzip gzip.c)
    target_link_libraries(libdeflate-gzip PRIVATE libdeflate_prog_utils)
    install(TARGETS libdeflate-gzip DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.14")
        # Install libdeflate-gunzip as a hard link to libdeflate-gzip.
        # Fall back to a copy if hard links are unsupported.
        #
        # Note: on Windows, prepending DESTDIR like this doesn't work correctly
        # when ${CMAKE_INSTALL_FULL_BINDIR} includes a drive letter.  But that
        # is fine since DESTDIR is unsupported on Windows anyway, according to
        # the CMake documentation.
        set(GZIP "${CMAKE_INSTALL_FULL_BINDIR}/libdeflate-gzip${CMAKE_EXECUTABLE_SUFFIX}")
        set(GUNZIP "${CMAKE_INSTALL_FULL_BINDIR}/libdeflate-gunzip${CMAKE_EXECUTABLE_SUFFIX}")
        install(CODE "message(\"-- Installing: \$ENV{DESTDIR}${GUNZIP}\")")
        install(CODE "file(CREATE_LINK \"\$ENV{DESTDIR}${GZIP}\"
                           \"\$ENV{DESTDIR}${GUNZIP}\" COPY_ON_ERROR)")
    else()
        # The cmake version is too old to support file(CREATE_LINK).
        # Just compile gzip.c again to build libdeflate-gunzip.
        add_executable(libdeflate-gunzip gzip.c)
        target_link_libraries(libdeflate-gunzip PRIVATE libdeflate_prog_utils)
        install(TARGETS libdeflate-gunzip DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endif()

# Build the test programs, if requested.
if(LIBDEFLATE_BUILD_TESTS)

    # The test programs depend on zlib for comparison tests.
    find_package(ZLIB REQUIRED)

    # Build a utility library for the test programs.
    add_library(libdeflate_test_utils STATIC test_util.c)
    set_target_properties(libdeflate_test_utils PROPERTIES
                          OUTPUT_NAME deflate_test_utils)
    target_link_libraries(libdeflate_test_utils PUBLIC
                          libdeflate_prog_utils ZLIB::ZLIB)

    # Build the benchmark and checksum programs.
    add_executable(benchmark benchmark.c)
    target_link_libraries(benchmark PRIVATE libdeflate_test_utils)
    add_executable(checksum checksum.c)
    target_link_libraries(checksum PRIVATE libdeflate_test_utils)

    # Build the unit test programs and register them with CTest.
    set(UNIT_TEST_PROGS
        test_checksums
        test_custom_malloc
        test_incomplete_codes
        test_invalid_streams
        test_litrunlen_overflow
        test_overread
        test_slow_decompression
        test_trailing_bytes
    )
    foreach(PROG ${UNIT_TEST_PROGS})
        add_executable(${PROG} ${PROG}.c)
        target_link_libraries(${PROG} PRIVATE libdeflate_test_utils)
        add_test(NAME ${PROG} COMMAND ${PROG})
    endforeach()
endif()
