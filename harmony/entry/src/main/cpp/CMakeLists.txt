cmake_minimum_required(VERSION 3.5.0)
project(bodyclock_napi)

# The HarmonyOS native module: libbodyclock.so = the NAPI bridge (napi_init.cpp) +
# bodyclock_core, pulled in by add_subdirectory on the repo root.
#
# The core's third-party dependencies come prebuilt from harmony/prebuilt/<abi>/
# -- run harmony/build.cmd (or .sh) once per ABI before building the app, or
# configuring fails on the first find_package.

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${NATIVERENDER_ROOT_PATH})

# entry/src/main/cpp -> main -> src -> entry -> harmony -> repo root
get_filename_component(YOAI_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
# harmony/prebuilt/ is the one home for prebuilt native artifacts, from the two
# halves of harmony/build: <abi>/ for the core's vcpkg dependencies (`deps`) and
# llama-sdk/<abi>/ for the on-device engine (`llama`, resolved further down).
# Named once here so the shared parent is visible in the code rather than implied
# by two similar strings.
set(YOAI_PREBUILT_ROOT "${YOAI_REPO_ROOT}/harmony/prebuilt")
set(YOAI_PREBUILT      "${YOAI_PREBUILT_ROOT}/${OHOS_ARCH}")

if(NOT EXISTS "${YOAI_PREBUILT}/lib")
    message(FATAL_ERROR
        "No prebuilt dependencies for ${OHOS_ARCH} at ${YOAI_PREBUILT}.\n"
        "Run harmony/build.cmd ${OHOS_ARCH} (or ./build.sh ${OHOS_ARCH}) first.")
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

# On-device LLM: llama.cpp cross-compiled for this ABI, linked STATICALLY into
# libbodyclock.so. A HAP has nowhere to put a loose companion .so, and the device will
# not exec a standalone binary anyway, so in-process is the only way to run a
# model here.
#
# The SDK is discovered exactly like the prebuilt tree above -- by repo-relative
# path, under the same prebuilt/ parent, at prebuilt/llama-sdk/<abi>, where
# `harmony/build.cmd <abi> llama` (or .sh) assembles it. So BUILDING IT IS
# TURNING THE LANE ON: there is no machine-local absolute path to paste into the
# tracked build-profile.json5 and then have to keep out of commits.
#
# Absent, the core compiles its stub and the app still builds, with on-device
# chat reporting itself unavailable. That is the supported off state, and it is
# what every clone gets until someone runs the script.
#
# An explicit -DLLAMA_CPP_DIR still wins, for an SDK assembled elsewhere.
# Probing include/llama.h rather than the directory: a half-assembled SDK should
# fail loudly in the root CMakeLists, which checks the archives and the API level
# too, not be silently ignored here.
if(NOT DEFINED LLAMA_CPP_DIR)
    set(_llama_sdk "${YOAI_PREBUILT_ROOT}/llama-sdk/${OHOS_ARCH}")
    # Re-configure when the SDK appears or goes away. Without this the probe runs
    # only on a configure that something ELSE triggered, so deleting the SDK
    # leaves a generated build.ninja still naming libllama.a as a link input and
    # the next incremental build dies with a raw "missing and no known rule to
    # make it" instead of turning the lane off. Both paths are listed: the marker
    # file covers a built SDK being removed, the parent directory covers one
    # appearing where there was none.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${YOAI_PREBUILT_ROOT}/llama-sdk"
                 "${_llama_sdk}/include/llama.h")
    if(EXISTS "${_llama_sdk}/include/llama.h")
        set(LLAMA_CPP_DIR "${_llama_sdk}")
    endif()
endif()
if(DEFINED LLAMA_CPP_DIR)
    set(YOAI_ONDEVICE_LLM ON CACHE BOOL "" FORCE)
    # Speech recognition rides on the SAME SDK and costs much less: it links the
    # ggml archives and never touches libllama, so anything that can build the
    # on-device LLM can build this for free. There is no separate switch on the
    # app side for the same reason there is none for the LLM -- the SDK's
    # presence is the switch.
    set(YOAI_ASR ON CACHE BOOL "" FORCE)
    message(STATUS "bodyclock_napi: on-device LLM and recognition from ${LLAMA_CPP_DIR}")
else()
    # FORCEd OFF, not merely left alone: now that the SDK's presence is the
    # switch, deleting it must actually turn the lane off. Without this a cached
    # ON from an earlier configure survives and the root CMakeLists fails
    # demanding LLAMA_CPP_DIR.
    set(YOAI_ONDEVICE_LLM OFF CACHE BOOL "" FORCE)
    set(YOAI_ASR OFF CACHE BOOL "" FORCE)
endif()

# The omni lane, and it is opt-in twice over: the SDK has to exist AND the
# capability has to be wanted. Unlike llama, whose presence is the switch, this
# one stays off unless harmony/build.cmd mnn has been run -- it is 3.8 GB of
# weights and a second quantization story bought for one capability whose cost
# has not been measured on a phone. See design/voice.md.
# Retired. src/voice/omni.cpp moved to src/voice/attic/ and is in no target, so
# an SDK found here would only change which rapidjson the JSON files compile
# against and link a framework nothing calls. Turning the lane back on means
# putting omni.cpp back in the root CMakeLists as well as uncommenting this.
#
# if(NOT DEFINED MNN_DIR)
#     set(_mnn_sdk "${YOAI_PREBUILT_ROOT}/mnn-sdk/${OHOS_ARCH}")
#     if(EXISTS "${_mnn_sdk}/include/llm/llm.hpp")
#         set(MNN_DIR "${_mnn_sdk}")
#     endif()
# endif()
# if(DEFINED MNN_DIR)
#     set(YOAI_MNN ON CACHE BOOL "" FORCE)
#     message(STATUS "bodyclock_napi: omni speech output from ${MNN_DIR}")
# else()
set(YOAI_MNN OFF CACHE BOOL "" FORCE)
# endif()

# Speech output, the third engine, discovered exactly like llama: the SDK's
# presence is the switch, and `harmony/build.cmd <abi> qtts` is what creates it.
#
# It also decides WHICH ggml the library links, because it ships one of its own
# and the root CMakeLists refuses to link two. That is settled there, not here.
if(NOT DEFINED QTTS_DIR)
    set(_qtts_sdk "${YOAI_PREBUILT_ROOT}/qtts-sdk/${OHOS_ARCH}")
    # Same reason as the llama probe: without this, deleting the SDK leaves a
    # build.ninja still naming libqwen-core.a as a link input.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${YOAI_PREBUILT_ROOT}/qtts-sdk"
                 "${_qtts_sdk}/include/qwen.h")
    if(EXISTS "${_qtts_sdk}/include/qwen.h")
        set(QTTS_DIR "${_qtts_sdk}")
    endif()
endif()
if(DEFINED QTTS_DIR)
    set(YOAI_QTTS ON CACHE BOOL "" FORCE)
    message(STATUS "bodyclock_napi: speech output from ${QTTS_DIR}")
else()
    set(YOAI_QTTS OFF CACHE BOOL "" FORCE)
endif()

add_subdirectory("${YOAI_REPO_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/bodyclock-core")

# The target is bodyclock_napi but the artifact must be libbodyclock.so: it has to match
# nm_modname in napi_init.cpp and the `import ... from 'libbodyclock.so'` on the
# ArkTS side. Keeping the CMake target name distinct also avoids colliding with
# the core's own target names.
add_library(bodyclock_napi SHARED napi_init.cpp)
set_target_properties(bodyclock_napi PROPERTIES OUTPUT_NAME bodyclock)

target_include_directories(bodyclock_napi PRIVATE "${YOAI_REPO_ROOT}/src")

# libace_napi: the N-API implementation. libhilog_ndk: native logging, so the
# worker thread's output lands in DevEco's Log window beside the ArkTS logs.
target_link_libraries(bodyclock_napi PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    bodyclock_core)
