cmake_minimum_required(VERSION 3.7)

# Default to a release build.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "No build type selected; defaulting to Release")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build" FORCE)
endif()

# With MSVC, don't automatically append /W3 to the compiler flags.
# This makes it possible for the user to select /W4.
if(POLICY CMP0092)
    cmake_policy(SET CMP0092 NEW)
endif()

# Extract the version string from libdeflate.h so that it doesn't have to be
# duplicated here.
set(VERSION_REGEX "#define LIBDEFLATE_VERSION_STRING[ \t]+\"([0-9\\.]+)\"")
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/libdeflate.h VERSION_STRING REGEX ${VERSION_REGEX})
string(REGEX REPLACE ${VERSION_REGEX} "\\1" VERSION_STRING "${VERSION_STRING}")

# Declare the project.
project(libdeflate
        LANGUAGES C
        VERSION ${VERSION_STRING})

# Include the CMake modules required by the top-level directory.
include(CMakePackageConfigHelpers)
include(CheckCCompilerFlag)
include(GNUInstallDirs)

# Declare the options, which can be overridden via 'cmake -DOPTION=VALUE'.
option(LIBDEFLATE_BUILD_STATIC_LIB "Build the static library" ON)
option(LIBDEFLATE_BUILD_SHARED_LIB "Build the shared library" ON)
option(LIBDEFLATE_COMPRESSION_SUPPORT "Support compression" ON)
option(LIBDEFLATE_DECOMPRESSION_SUPPORT "Support decompression" ON)
option(LIBDEFLATE_ZLIB_SUPPORT "Support the zlib format" ON)
option(LIBDEFLATE_GZIP_SUPPORT "Support the gzip format" ON)
option(LIBDEFLATE_FREESTANDING
       "Build a freestanding library, i.e. a library that doesn't link to any
       libc functions like malloc(), free(), and memcpy().  Library users will
       need to provide a custom memory allocator." OFF)
option(LIBDEFLATE_BUILD_GZIP "Build the libdeflate-gzip program" ON)
option(LIBDEFLATE_BUILD_TESTS "Build the test programs" OFF)
option(LIBDEFLATE_USE_SHARED_LIB
       "Link the libdeflate-gzip and test programs to the shared library instead
       of the static library" OFF)

if(LIBDEFLATE_BUILD_TESTS)
    enable_testing()
endif()

# The gzip program can't be built if any library feature it needs is disabled.
if(NOT LIBDEFLATE_COMPRESSION_SUPPORT OR NOT LIBDEFLATE_DECOMPRESSION_SUPPORT
   OR NOT LIBDEFLATE_GZIP_SUPPORT)
    set(LIBDEFLATE_BUILD_GZIP OFF)
endif()

# If the static library isn't being built, we have to link to the shared one.
if(NOT LIBDEFLATE_BUILD_STATIC_LIB)
    set(LIBDEFLATE_USE_SHARED_LIB ON)
endif()

# Set common C compiler flags for all targets (the library and the programs).
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_STANDARD 99)
if(NOT MSVC)
    check_c_compiler_flag(-Wdeclaration-after-statement HAVE_WDECLARATION_AFTER_STATEMENT)
    check_c_compiler_flag(-Wimplicit-fallthrough HAVE_WIMPLICIT_FALLTHROUGH)
    check_c_compiler_flag(-Wmissing-field-initializers HAVE_WMISSING_FIELD_INITIALIZERS)
    check_c_compiler_flag(-Wmissing-prototypes HAVE_WMISSING_PROTOTYPES)
    check_c_compiler_flag(-Wpedantic HAVE_WPEDANTIC)
    check_c_compiler_flag(-Wshadow HAVE_WSHADOW)
    check_c_compiler_flag(-Wstrict-prototypes HAVE_WSTRICT_PROTOTYPES)
    check_c_compiler_flag(-Wundef HAVE_WUNDEF)
    check_c_compiler_flag(-Wvla HAVE_WVLA)
    add_compile_options(
        -Wall
        $<$<BOOL:${HAVE_WDECLARATION_AFTER_STATEMENT}>:-Wdeclaration-after-statement>
        $<$<BOOL:${HAVE_WIMPLICIT_FALLTHROUGH}>:-Wimplicit-fallthrough>
        $<$<BOOL:${HAVE_WMISSING_FIELD_INITIALIZERS}>:-Wmissing-field-initializers>
        $<$<BOOL:${HAVE_WMISSING_PROTOTYPES}>:-Wmissing-prototypes>
        $<$<BOOL:${HAVE_WPEDANTIC}>:-Wpedantic>
        $<$<BOOL:${HAVE_WSHADOW}>:-Wshadow>
        $<$<BOOL:${HAVE_WSTRICT_PROTOTYPES}>:-Wstrict-prototypes>
        $<$<BOOL:${HAVE_WUNDEF}>:-Wundef>
        $<$<BOOL:${HAVE_WVLA}>:-Wvla>
    )
endif()
if(LIBDEFLATE_FREESTANDING)
    add_definitions(-DFREESTANDING)
endif()

# Check for cases where the compiler supports an instruction set extension but
# the assembler does not, and in those cases print a warning and add an
# appropriate -DLIBDEFLATE_ASSEMBLER_DOES_NOT_SUPPORT_* flag.  libdeflate's C
# source files already check the compiler version before using the corresponding
# intrinsics, but in the rare case of gcc being paired with a binutils much
# older than itself those checks are insufficient.  There is no way to check the
# assembler version from C.  The proper fix for too-old binutils is for the user
# to upgrade binutils.  Unfortunately, as libdeflate has started using newer
# instructions, binutils incompatibilities have started being seen more
# frequently.  Hence these checks for assembler support here in CMakeLists.txt
# to provide a fallback for users who may be unable to fix their toolchain.
# These don't solve the problem for users not using CMake, though such users can
# add specific -DLIBDEFLATE_ASSEMBLER_DOES_NOT_SUPPORT_* flags they need.
function(check_assembler_support  feature assembly_code)
    execute_process(COMMAND echo "${assembly_code}"
                    COMMAND ${CMAKE_C_COMPILER} -c -x assembler -o /dev/null -
                    RESULT_VARIABLE result
                    ERROR_QUIET)
    if(NOT ${result} EQUAL 0)
        add_definitions(-DLIBDEFLATE_ASSEMBLER_DOES_NOT_SUPPORT_${feature})
        message(STATUS "Your gcc supports ${feature} instructions but it is paired with an assembler that does not.  Upgrading binutils is recommended.")
    endif()
endfunction()
if(UNIX AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    execute_process(COMMAND ${CMAKE_C_COMPILER} -dumpmachine
                    OUTPUT_VARIABLE machine)
    if(${machine} MATCHES "^(x86_64|i[3-6]86)")
        if(${CMAKE_C_COMPILER_VERSION} VERSION_GREATER_EQUAL 8.1)
            # Set LIBDEFLATE_ASSEMBLER_DOES_NOT_SUPPORT_AVX512VNNI if needed.
            check_assembler_support(AVX512VNNI "vpdpbusd %zmm0, %zmm0, %zmm0")
            # Set LIBDEFLATE_ASSEMBLER_DOES_NOT_SUPPORT_VPCLMULQDQ if needed.
            check_assembler_support(VPCLMULQDQ "vpclmulqdq $0, %zmm0, %zmm0, %zmm0")
        endif()
        if(${CMAKE_C_COMPILER_VERSION} VERSION_GREATER_EQUAL 12.1)
            # Set LIBDEFLATE_ASSEMBLER_DOES_NOT_SUPPORT_AVX_VNNI if needed.
            check_assembler_support(AVX_VNNI "{vex} vpdpbusd %ymm0, %ymm0, %ymm0")
        endif()
    elseif(${machine} MATCHES "^aarch64")
        if(${CMAKE_C_COMPILER_VERSION} VERSION_GREATER_EQUAL 8.1)
            # Set LIBDEFLATE_ASSEMBLER_DOES_NOT_SUPPORT_DOTPROD if needed.
            check_assembler_support(DOTPROD ".arch armv8.2-a+dotprod\nudot v0.4s, v0.16b, v0.16b")
        endif()
        if(${CMAKE_C_COMPILER_VERSION} VERSION_GREATER_EQUAL 9.1)
            # Set LIBDEFLATE_ASSEMBLER_DOES_NOT_SUPPORT_SHA3 if needed.
            check_assembler_support(SHA3 ".arch armv8.2-a+sha3\neor3 v0.16b, v0.16b, v0.16b, v0.16b")
        endif()
    endif()
endif()

# Determine the list of source files and the list of compiler options that will
# be used for both the static library and the shared library.

set(LIB_SOURCES
    common_defs.h
    libdeflate.h
    lib/arm/cpu_features.c
    lib/arm/cpu_features.h
    lib/cpu_features_common.h
    lib/deflate_constants.h
    lib/lib_common.h
    lib/utils.c
    lib/x86/cpu_features.c
    lib/x86/cpu_features.h
)
if(LIBDEFLATE_COMPRESSION_SUPPORT)
    list(APPEND LIB_SOURCES
         lib/arm/matchfinder_impl.h
         lib/bt_matchfinder.h
         lib/deflate_compress.c
         lib/deflate_compress.h
         lib/hc_matchfinder.h
         lib/ht_matchfinder.h
         lib/matchfinder_common.h
         lib/riscv/matchfinder_impl.h
         lib/x86/matchfinder_impl.h
    )
endif()
if(LIBDEFLATE_DECOMPRESSION_SUPPORT)
    list(APPEND LIB_SOURCES
         lib/decompress_template.h
         lib/deflate_decompress.c
         lib/x86/decompress_impl.h
    )
endif()
if(LIBDEFLATE_ZLIB_SUPPORT)
    list(APPEND LIB_SOURCES
         lib/adler32.c
         lib/arm/adler32_impl.h
         lib/x86/adler32_impl.h
         lib/x86/adler32_template.h
         lib/zlib_constants.h
    )
    if(LIBDEFLATE_COMPRESSION_SUPPORT)
        list(APPEND LIB_SOURCES lib/zlib_compress.c)
    endif()
    if(LIBDEFLATE_DECOMPRESSION_SUPPORT)
        list(APPEND LIB_SOURCES lib/zlib_decompress.c)
    endif()
endif()
if(LIBDEFLATE_GZIP_SUPPORT)
    list(APPEND LIB_SOURCES
         lib/arm/crc32_impl.h
         lib/arm/crc32_pmull_helpers.h
         lib/arm/crc32_pmull_wide.h
         lib/crc32.c
         lib/crc32_multipliers.h
         lib/crc32_tables.h
         lib/gzip_constants.h
         lib/x86/crc32_impl.h
         lib/x86/crc32_pclmul_template.h
    )
    if(LIBDEFLATE_COMPRESSION_SUPPORT)
        list(APPEND LIB_SOURCES lib/gzip_compress.c)
    endif()
    if(LIBDEFLATE_DECOMPRESSION_SUPPORT)
        list(APPEND LIB_SOURCES lib/gzip_decompress.c)
    endif()
endif()

if(LIBDEFLATE_FREESTANDING)
    list(APPEND LIB_COMPILE_OPTIONS -ffreestanding -nostdlib)
    list(APPEND LIB_LINK_LIBRARIES -ffreestanding -nostdlib)
endif()

set(LIB_INCLUDE_DIRS
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_FULL_INCLUDEDIR}>)

# Build the static library.
if(LIBDEFLATE_BUILD_STATIC_LIB)
    add_library(libdeflate_static STATIC ${LIB_SOURCES})

    # This alias allows third-party usage of the library with CMake to work the
    # same way with add_subdirectory() as with other ways.
    add_library(libdeflate::libdeflate_static ALIAS libdeflate_static)

    if(WIN32 AND NOT MINGW)
        set(STATIC_LIB_NAME deflatestatic)
    else()
        set(STATIC_LIB_NAME deflate)
    endif()
    set_target_properties(libdeflate_static PROPERTIES
                          OUTPUT_NAME ${STATIC_LIB_NAME}
                          PUBLIC_HEADER libdeflate.h)
    target_include_directories(libdeflate_static PUBLIC ${LIB_INCLUDE_DIRS})
    target_compile_definitions(libdeflate_static PRIVATE ${LIB_COMPILE_DEFINITIONS})
    target_compile_options(libdeflate_static PRIVATE ${LIB_COMPILE_OPTIONS})
    list(APPEND LIB_TARGETS libdeflate_static)
endif()

# Build the shared library.
if(LIBDEFLATE_BUILD_SHARED_LIB)
    add_library(libdeflate_shared SHARED ${LIB_SOURCES})

    # This alias allows third-party usage of the library with CMake to work the
    # same way with add_subdirectory() as with other ways.
    add_library(libdeflate::libdeflate_shared ALIAS libdeflate_shared)

    set_target_properties(libdeflate_shared PROPERTIES
                          OUTPUT_NAME deflate
                          PUBLIC_HEADER libdeflate.h
                          C_VISIBILITY_PRESET hidden
                          SOVERSION 0)
    target_include_directories(libdeflate_shared PUBLIC ${LIB_INCLUDE_DIRS})
    target_compile_definitions(libdeflate_shared PUBLIC LIBDEFLATE_DLL)
    target_compile_definitions(libdeflate_shared PRIVATE ${LIB_COMPILE_DEFINITIONS})
    target_compile_options(libdeflate_shared PRIVATE ${LIB_COMPILE_OPTIONS})
    target_link_libraries(libdeflate_shared PRIVATE ${LIB_LINK_LIBRARIES})
    list(APPEND LIB_TARGETS libdeflate_shared)
endif()

# Install the static and/or shared library.
install(TARGETS ${LIB_TARGETS}
        EXPORT libdeflate_exported_targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Generate and install the pkg-config file.  (Don't confuse this with the CMake
# package config file, which is CMake-specific.)  Take care to define the
# include and lib directories in terms of the ${prefix} and ${exec_prefix}
# pkg-config variables when possible, since some pkg-config users expect to be
# able to override these variables to relocate packages.
if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(CMAKE_PKGCONFIG_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
else()
    set(CMAKE_PKGCONFIG_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
endif()
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(CMAKE_PKGCONFIG_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
else()
    set(CMAKE_PKGCONFIG_LIBDIR "\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}")
endif()
configure_file(libdeflate.pc.in libdeflate.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libdeflate.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# Generate a "libdeflate-targets.cmake" file in the build tree that can be
# included by outside projects to import targets from the build tree.
export(EXPORT libdeflate_exported_targets
       NAMESPACE libdeflate::
       FILE libdeflate-targets.cmake)

# Generate and install a separate "libdeflate-targets.cmake" file that can be
# included by outside projects to import targets from the installation tree.
install(EXPORT libdeflate_exported_targets
        NAMESPACE libdeflate::
        FILE libdeflate-targets.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libdeflate)

# Generate and install the CMake package version and config files.
write_basic_package_version_file(libdeflate-config-version.cmake
                                 VERSION ${PROJECT_VERSION}
                                 COMPATIBILITY AnyNewerVersion)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/libdeflate-config.cmake.in
    libdeflate-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libdeflate)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libdeflate-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/libdeflate-config-version.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libdeflate)

# Build the programs subdirectory if needed.
if(LIBDEFLATE_BUILD_GZIP OR LIBDEFLATE_BUILD_TESTS)
    add_subdirectory(programs)
endif()
