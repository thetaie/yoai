cmake_minimum_required(VERSION 3.5.0)
project(yoai_napi)

# The HarmonyOS native module: libyoai.so = the NAPI bridge (napi_init.cpp) +
# yoai_core, pulled in by add_subdirectory on the repo root.
#
# The core's third-party dependencies come prebuilt from harmony/prebuilt/<abi>/
# -- run harmony/build-prebuilt.cmd (or .sh) once per ABI before building the
# app, or configuring fails on the first find_package.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${NATIVERENDER_ROOT_PATH})

# entry/src/main/cpp -> main -> src -> entry -> harmony -> repo root
get_filename_component(YOAI_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(YOAI_PREBUILT "${YOAI_REPO_ROOT}/harmony/prebuilt/${OHOS_ARCH}")

if(NOT EXISTS "${YOAI_PREBUILT}/lib")
    message(FATAL_ERROR
        "No prebuilt dependencies for ${OHOS_ARCH} at ${YOAI_PREBUILT}.\n"
        "Run harmony/build-prebuilt.cmd ${OHOS_ARCH} (or ./build-prebuilt.sh ${OHOS_ARCH}) first.")
endif()

# find_package() inside the core resolves against the prebuilt tree.
# FIND_ROOT_PATH matters because the OHOS toolchain sets
# CMAKE_FIND_ROOT_PATH_MODE_* to ONLY, so a plain PREFIX_PATH is not consulted.
list(PREPEND CMAKE_PREFIX_PATH    "${YOAI_PREBUILT}")
list(PREPEND CMAKE_FIND_ROOT_PATH "${YOAI_PREBUILT}")

# Cached + FORCE: the root CMakeLists declares this as an option(), so a plain
# set() would lose to the cache on the second configure. No CLI harness on a
# device -- a HAP has nowhere to put a companion executable.
set(YOAI_BUILD_CLI OFF CACHE BOOL "" FORCE)

add_subdirectory("${YOAI_REPO_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/yoai-core")

# The target is yoai_napi but the artifact must be libyoai.so: it has to match
# nm_modname in napi_init.cpp and the `import ... from 'libyoai.so'` on the
# ArkTS side. Keeping the CMake target name distinct also avoids colliding with
# the core's own target names.
add_library(yoai_napi SHARED napi_init.cpp)
set_target_properties(yoai_napi PROPERTIES OUTPUT_NAME yoai)

target_include_directories(yoai_napi PRIVATE "${YOAI_REPO_ROOT}/src")

# libace_napi: the N-API implementation. libhilog_ndk: native logging, so the
# worker thread's output lands in DevEco's Log window beside the ArkTS logs.
target_link_libraries(yoai_napi PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    yoai_core)
