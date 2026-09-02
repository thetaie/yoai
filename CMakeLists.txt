cmake_minimum_required(VERSION 3.19)
project(yoai CXX)

# The YoAI core: a static library plus a C ABI (src/yoai.h), built to be linked
# INTO a host app -- the HarmonyOS native module today, an Android one next --
# rather than run as a process. There is no server here and no port to bind: the
# host owns storage and settings and reaches the core through the C ABI.
#
# A desktop CLI (cli/) links the same library, so the core can be exercised on a
# workstation without a device in the loop. It is a test harness, not a product.
#
# Dependencies come from vcpkg (see vcpkg.json). Cross-compiling for HarmonyOS
# goes through harmony/build-prebuilt.{sh,cmd}, which lays the same ports out
# under harmony/prebuilt/<abi>/ for the find_package calls below to resolve.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)   # linked into a shared host module

if(MSVC)
    add_compile_options(/utf-8 /W3)
    # MSVC deprecates the standard C library calls in favour of its own _s
    # variants, which nothing else has. The portable spellings are the ones we
    # want on three platforms.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra)
endif()

#-------------------------------------------------------------------------------
# Dependencies
#-------------------------------------------------------------------------------

find_package(CURL REQUIRED)

# Not called directly anywhere in the core -- curl is what talks TLS. Resolved
# and linked anyway because a static curl (every mobile build) leaves its
# OpenSSL dependency for the final link to satisfy.
find_package(OpenSSL REQUIRED)

# Header-only, and packaged inconsistently: vcpkg ships a config file, most
# distro packages ship only headers.
find_package(RapidJSON CONFIG QUIET)
if(NOT RapidJSON_FOUND)
    find_path(RAPIDJSON_INCLUDE_DIR rapidjson/document.h REQUIRED)
endif()

#-------------------------------------------------------------------------------
# yoai_core
#-------------------------------------------------------------------------------

add_library(yoai_core STATIC
    src/client/http_client.cpp
    src/llm/event.cpp
    src/llm/openai_chat.cpp
    src/llm/sse_parser.cpp
    src/platform/c_api.cpp
    src/platform/log.cpp
)

target_include_directories(yoai_core PUBLIC src)

if(RapidJSON_FOUND)
    if(TARGET rapidjson)
        target_link_libraries(yoai_core PUBLIC rapidjson)
    else()
        target_include_directories(yoai_core PUBLIC ${RAPIDJSON_INCLUDE_DIRS})
    endif()
else()
    target_include_directories(yoai_core PUBLIC ${RAPIDJSON_INCLUDE_DIR})
endif()

target_link_libraries(yoai_core PUBLIC
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
)

# platform/log.cpp routes to the platform's own log sink where there is one --
# stderr reaches nobody on a phone.
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(yoai_core PUBLIC log)
elseif(OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    target_link_libraries(yoai_core PUBLIC libhilog_ndk.z.so)
endif()

#-------------------------------------------------------------------------------
# cli -- a desktop harness for the core
#-------------------------------------------------------------------------------

# Off when cross-compiling: a HAP has nowhere to put a companion executable, and
# the device refuses to exec a standalone binary anyway.
if(CMAKE_CROSSCOMPILING)
    set(_yoai_cli_default OFF)
else()
    set(_yoai_cli_default ON)
endif()
option(YOAI_BUILD_CLI "Build the desktop CLI harness (cli/)" ${_yoai_cli_default})

if(YOAI_BUILD_CLI)
    add_executable(yoai-cli cli/main.cpp)
    target_link_libraries(yoai-cli PRIVATE yoai_core)
endif()
