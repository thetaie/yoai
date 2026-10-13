cmake_minimum_required(VERSION 3.19)
project(yoai_server CXX)

# yoai-server: text in, speech out, over a WebSocket. yoai_core plus
# src/tts_server.cpp and src/main.cpp, linked into one executable.
#
# It links the repo's root CMakeLists rather than copying any of it, as
# examples/desktop/native does, so a change to the core reaches the server by
# rebuilding and nothing else. Driven by build.py beside this file, which
# passes the vcpkg toolchain, with the manifest's `server` feature for
# libwebsockets, and QTTS_DIR, the speech SDK.

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#-------------------------------------------------------------------------------
# The core
#-------------------------------------------------------------------------------
#
# The CLI harness, its tests and the shared library are not wanted here, so
# they are turned off before the subdirectory is read -- FORCE because they are
# cached options and a stale ON would survive.
#
# Speech output is turned ON the same way: the server has nothing else to
# serve. The root file then requires QTTS_DIR, and says so when it is missing.
set(YOAI_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(YOAI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(YOAI_QTTS ON CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." "${CMAKE_CURRENT_BINARY_DIR}/yoai-core")

#-------------------------------------------------------------------------------
# libwebsockets
#-------------------------------------------------------------------------------

find_package(libwebsockets CONFIG REQUIRED)
# The shared library where there is one -- the only one a dynamic vcpkg
# triplet builds -- and the static one otherwise.
if(TARGET websockets_shared)
    set(_yoai_lws websockets_shared)
elseif(TARGET websockets)
    set(_yoai_lws websockets)
else()
    message(FATAL_ERROR "libwebsockets was found but exports neither websockets_shared "
                        "nor websockets.")
endif()

#-------------------------------------------------------------------------------
# The server
#-------------------------------------------------------------------------------

add_executable(yoai-server src/main.cpp src/tts_server.cpp)

# ../../cli for console.hpp, the UTF-8 arguments the CLI tools read too.
target_include_directories(yoai-server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../cli"
    ${LIBWEBSOCKETS_INCLUDE_DIRS})
target_link_libraries(yoai-server PRIVATE yoai_core ${_yoai_lws})

if(MSVC)
    target_compile_options(yoai-server PRIVATE /utf-8 /W3)
    target_compile_definitions(yoai-server PRIVATE _CRT_SECURE_NO_WARNINGS)
    # CommandLineToArgvW, which console.hpp uses to read the arguments as UTF-8.
    target_link_libraries(yoai-server PRIVATE shell32)
else()
    target_compile_options(yoai-server PRIVATE -Wall -Wextra)
endif()

message(STATUS "yoai-server: libwebsockets -> ${_yoai_lws}")
