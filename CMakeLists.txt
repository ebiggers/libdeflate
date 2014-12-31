cmake_minimum_required(VERSION 2.6)
project(libdeflate C)

set(LIB_VERSION_MAJOR 0)
set(LIB_VERSION_MINOR 0)
set(LIB_VERSION_PATCH 0)

set(LIB_VERSION_STRING "${LIB_VERSION_MAJOR}.${LIB_VERSION_MINOR}.${LIB_VERSION_PATCH}")

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(C_FLAGS "-std=c11 -fvisibility=hidden")

set(CMAKE_C_FLAGS_RELEASE "${C_FLAGS} -O2 -DNDEBUG")
set(CMAKE_C_FLAGS_DEBUG "${C_FLAGS} -O0 -g")

include_directories(".")

option(SUPPORT_COMPRESSION "Support DEFLATE compression" ON)
if(SUPPORT_COMPRESSION)
	set(LIB_SOURCES ${LIB_SOURCES} src/deflate_compress.c)
endif()

option(SUPPORT_DECOMPRESSION "Support DEFLATE decompression" ON)
if(SUPPORT_DECOMPRESSION)
	set(LIB_SOURCES ${LIB_SOURCES} src/deflate_decompress.c)
endif()

option(SUPPORT_ZLIB "Support zlib wrapper format" ON)
if(SUPPORT_ZLIB)
	set(LIB_SOURCES ${LIB_SOURCES} src/adler32.c)
	if(SUPPORT_COMPRESSION)
		set(LIB_SOURCES ${LIB_SOURCES} src/zlib_compress.c)
	endif()
	if(SUPPORT_DECOMPRESSION)
		set(LIB_SOURCES ${LIB_SOURCES} src/zlib_decompress.c)
	endif()
endif()

option(SUPPORT_GZIP "Support gzip wrapper format" ON)
if(SUPPORT_GZIP)
	set(LIB_SOURCES ${LIB_SOURCES} src/crc32.c)
	if(SUPPORT_COMPRESSION)
		set(LIB_SOURCES ${LIB_SOURCES} src/gzip_compress.c)
	endif()
	if(SUPPORT_DECOMPRESSION)
		set(LIB_SOURCES ${LIB_SOURCES} src/gzip_decompress.c)
	endif()
endif()

option(SUPPORT_NEAR_OPTIMAL_PARSING "Support near optimal parsing (high compression mode)" ON)
if(SUPPORT_NEAR_OPTIMAL_PARSING)
	add_definitions(-DSUPPORT_NEAR_OPTIMAL_PARSING=1)
else()
	add_definitions(-DSUPPORT_NEAR_OPTIMAL_PARSING=0)
endif()

option(UNSAFE_DECOMPRESSION "Assume that all compressed data is valid (faster but insecure)" OFF)
if(UNSAFE_DECOMPRESSION)
	add_definitions(-DUNSAFE_DECOMPRESSION=1)
else()
	add_definitions(-DUNSAFE_DECOMPRESSION=0)
endif()

add_library(deflate SHARED ${LIB_SOURCES})
add_library(deflatestatic STATIC ${LIB_SOURCES})

set_target_properties(deflate PROPERTIES VERSION ${LIB_VERSION_STRING})
set_target_properties(deflate PROPERTIES SOVERSION ${LIB_VERSION_MAJOR})

install(TARGETS deflate deflatestatic
	LIBRARY DESTINATION "${CMAKE_INSTALL_PREFIX}/lib"
	ARCHIVE DESTINATION "${CMAKE_INSTALL_PREFIX}/lib")

install(FILES libdeflate.h DESTINATION "${CMAKE_INSTALL_PREFIX}/include")

option(BUILD_BENCHMARK "Build benchmark program" OFF)
add_executable(benchmark test/benchmark.c)
target_link_libraries(benchmark deflate -lz)
