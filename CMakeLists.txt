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
# goes through harmony/build.{sh,cmd}, which lays the same ports out
# under harmony/prebuilt/<abi>/ for the find_package calls below to resolve.

set(CMAKE_CXX_STANDARD 11)
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
    # Compiles to a stub unless YOAI_ONDEVICE_LLM is on (see below), so the
    # default build needs no llama.cpp and still exposes the on-device entry
    # points -- they just answer "not built in".
    src/llm/local.cpp
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
# On-device LLM (llama.cpp + GGUF)
#-------------------------------------------------------------------------------

# OFF by default: the engine is a few hundred MB of static archives that only the
# mobile hosts want, and src/llm/local.cpp compiles a stub without it, so the
# desktop build stays a two-minute vcpkg affair.
#
# LLAMA_CPP_DIR points at an SDK dir: include/llama.h + lib/. Both layouts work --
# a shared build (one libllama) and a static build (libllama plus the ggml
# archives), which is what a HAP needs since there is nowhere to put a companion
# .so. The ggml libs are therefore optional.
#
# On HarmonyOS nobody sets either variable by hand: `harmony/build.cmd <abi>
# llama` (or .sh) assembles the SDK at harmony/prebuilt/llama-sdk/<abi> and the
# app's CMakeLists finds it there. Building the SDK IS turning the lane on.
option(YOAI_ONDEVICE_LLM "Build the on-device LLM engine (llama.cpp + GGUF)" OFF)

if(YOAI_ONDEVICE_LLM)
    if(NOT DEFINED LLAMA_CPP_DIR)
        message(FATAL_ERROR
            "YOAI_ONDEVICE_LLM=ON requires -DLLAMA_CPP_DIR=<llama.cpp SDK dir (include/, lib/)>")
    endif()

    # find_library caches, so without clearing it a changed LLAMA_CPP_DIR
    # (switching target ABI) would silently keep linking the previously found libs.
    foreach(_v LLAMA_LIB GGML_LIB GGML_CPU_LIB GGML_BASE_LIB)
        unset(${_v} CACHE)
    endforeach()

    # NO_CMAKE_FIND_ROOT_PATH is required for the cross build: the OHOS toolchain
    # sets CMAKE_FIND_ROOT_PATH_MODE_LIBRARY to ONLY, which re-roots PATHS under
    # the sysroot and makes an absolute SDK dir unfindable.
    set(_llama_find PATHS "${LLAMA_CPP_DIR}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_library(LLAMA_LIB     NAMES llama     ${_llama_find})
    find_library(GGML_LIB      NAMES ggml      ${_llama_find})
    find_library(GGML_CPU_LIB  NAMES ggml-cpu  ${_llama_find})
    find_library(GGML_BASE_LIB NAMES ggml-base ${_llama_find})

    if(NOT LLAMA_LIB)
        message(FATAL_ERROR
            "llama library not found under ${LLAMA_CPP_DIR}/lib. Check LLAMA_CPP_DIR.")
    endif()
    if(NOT EXISTS "${LLAMA_CPP_DIR}/include/llama.h")
        message(FATAL_ERROR "llama.h not found under ${LLAMA_CPP_DIR}/include.")
    endif()

    # The SDK's API LEVEL, not just its presence. src/llm/local.cpp passes the
    # n_vocab first argument to llama_sampler_init_penalties, which upstream
    # restored to that signature in 2026; the four-argument form had stood since
    # 2024-12. Against an older SDK everything else compiles and only that one call
    # fails -- as "requires 5 arguments, but 4 were provided", which reads like a
    # bug in our code rather than a stale dependency. Worth checking because an
    # assembled SDK outlives its source: the build script reuses one whenever the
    # archives are merely present, and never updates an existing llama.cpp clone.
    file(READ "${LLAMA_CPP_DIR}/include/llama.h" _llama_h)
    string(REGEX REPLACE "[ \t\r\n]+" "" _llama_h_flat "${_llama_h}")
    if(NOT _llama_h_flat MATCHES "llama_sampler_init_penalties\\(int32_tn_vocab")
        message(FATAL_ERROR
            "The llama.cpp SDK at ${LLAMA_CPP_DIR} is older than this repo needs: its "
            "llama_sampler_init_penalties has no n_vocab parameter, so src/llm/local.cpp "
            "will not compile against it.\n"
            "Update the llama.cpp checkout and re-run harmony/build.cmd <abi> llama (or .sh) "
            "with `clean`, so the cached SDK is discarded rather than reused.")
    endif()
    unset(_llama_h)
    unset(_llama_h_flat)

    target_compile_definitions(yoai_core PUBLIC YOAI_ONDEVICE_LLM)
    target_include_directories(yoai_core PRIVATE "${LLAMA_CPP_DIR}/include")

    # llama first: with static archives the linker resolves left to right, and
    # llama references ggml, not the other way round.
    target_link_libraries(yoai_core PUBLIC "${LLAMA_LIB}")
    foreach(_lib GGML_LIB GGML_CPU_LIB GGML_BASE_LIB)
        if(${_lib})
            target_link_libraries(yoai_core PUBLIC "${${_lib}}")
        endif()
    endforeach()

    message(STATUS "yoai_core: on-device LLM ON -> ${LLAMA_LIB}")
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
