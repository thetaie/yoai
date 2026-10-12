cmake_minimum_required(VERSION 3.19)
project(yoai_jni)

# The Android native module: libyoai.so = the JNI bridge (jni_init.cpp) +
# yoai_core, pulled in by add_subdirectory on the repo root.
#
# This is the Android counterpart of examples/harmony/entry/src/main/cpp/CMakeLists.txt
# and follows its one rule: BUILDING AN SDK IS TURNING ITS LANE ON. Nothing
# here takes a machine-local absolute path, so a tracked build file never has
# to be kept out of commits.
#
# The core's third-party dependencies come prebuilt from examples/android/prebuilt/<abi>/
# -- run examples/android/build.py once per ABI before building the app, or
# configuring fails on the first find_package.

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# app/src/main/cpp -> main -> src -> app -> android -> examples -> repo root
get_filename_component(YOAI_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
set(YOAI_PREBUILT_ROOT "${YOAI_REPO_ROOT}/examples/android/prebuilt")

# ANDROID_ABI is set by the NDK toolchain file. YOAI_ANDROID_ABI comes from
# the Gradle property and is only a cross-check: a mismatch means the ABI filter
# and the prebuilt tree have drifted apart, which otherwise surfaces as a link
# error naming an architecture instead of a configuration.
if(DEFINED YOAI_ANDROID_ABI AND NOT YOAI_ANDROID_ABI STREQUAL ANDROID_ABI)
    message(FATAL_ERROR
        "ABI mismatch: Gradle asked for ${YOAI_ANDROID_ABI}, the toolchain "
        "is building ${ANDROID_ABI}.")
endif()

set(YOAI_PREBUILT "${YOAI_PREBUILT_ROOT}/${ANDROID_ABI}")

if(NOT EXISTS "${YOAI_PREBUILT}/lib")
    message(FATAL_ERROR
        "No prebuilt dependencies for ${ANDROID_ABI} at ${YOAI_PREBUILT}.\n"
        "Run python examples/android/build.py ${ANDROID_ABI} deps first.")
endif()

# find_package() inside the core resolves against the prebuilt tree.
# FIND_ROOT_PATH matters because the NDK toolchain sets
# CMAKE_FIND_ROOT_PATH_MODE_* to ONLY, so a plain PREFIX_PATH is not consulted.
list(PREPEND CMAKE_PREFIX_PATH    "${YOAI_PREBUILT}")
list(PREPEND CMAKE_FIND_ROOT_PATH "${YOAI_PREBUILT}")

# Cached + FORCE: the root CMakeLists declares this as an option(), so a plain
# set() would lose to the cache on the second configure. No CLI harness in an
# APK -- there is nowhere to put a companion executable that the system will
# exec.
set(YOAI_BUILD_CLI OFF CACHE BOOL "" FORCE)

# The omni lane is retired; src/voice/omni.cpp is in src/voice/attic/ and in no
# target. Held OFF explicitly so a cached ON from nowhere cannot reach the root.
set(YOAI_MNN OFF CACHE BOOL "" FORCE)

#-------------------------------------------------------------------------------
# On-device LLM and recognition: llama.cpp, cross-compiled for this ABI
#-------------------------------------------------------------------------------
#
# Linked STATICALLY into libyoai.so. An APK can ship loose .so files, but
# the core links ggml archives directly for recognition, so one library is both
# simpler and what the HarmonyOS lane already does.
#
# Absent, the core compiles its stub and the app still builds, reporting the
# lane unavailable. That is the supported off state, and it is what every clone
# gets until someone runs the script.
#
# An explicit -DLLAMA_CPP_DIR still wins, for an SDK assembled elsewhere.
# Re-configure when an SDK appears or goes away, for both lanes at once.
#
# **The watched path has to exist at configure time.** CMake drops a missing
# entry from CMAKE_CONFIGURE_DEPENDS rather than watching for its creation, so
# naming the SDK directory itself covers only the case where one is DELETED --
# building the first SDK leaves a build.ninja that never learns about it, and
# the app silently keeps its stubs. Measured here: the lane stayed off across a
# full assembleDebug with the SDK sitting in place.
#
# prebuilt/ always exists by this point (the check above is fatal without it)
# and creating prebuilt/llama-sdk/ inside it updates its mtime, so this catches
# an SDK arriving. The specific headers are listed as well, for the reverse.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${YOAI_PREBUILT_ROOT}"
             "${YOAI_PREBUILT_ROOT}/llama-sdk/${ANDROID_ABI}/include/llama.h"
             "${YOAI_PREBUILT_ROOT}/qtts-sdk/${ANDROID_ABI}/include/qwen.h")

if(NOT DEFINED LLAMA_CPP_DIR)
    set(_llama_sdk "${YOAI_PREBUILT_ROOT}/llama-sdk/${ANDROID_ABI}")
    if(EXISTS "${_llama_sdk}/include/llama.h")
        set(LLAMA_CPP_DIR "${_llama_sdk}")
    endif()
endif()
if(DEFINED LLAMA_CPP_DIR)
    set(YOAI_ONDEVICE_LLM ON CACHE BOOL "" FORCE)
    # Recognition rides on the SAME SDK and costs much less: it links the ggml
    # archives and never touches libllama, so anything that can build the
    # on-device LLM can build this for free.
    set(YOAI_ASR ON CACHE BOOL "" FORCE)
    message(STATUS "yoai_jni: on-device LLM and recognition from ${LLAMA_CPP_DIR}")
else()
    # FORCEd OFF, not merely left alone: the SDK's presence is the switch, so
    # deleting it must actually turn the lane off. Without this a cached ON from
    # an earlier configure survives and the root CMakeLists fails demanding
    # LLAMA_CPP_DIR.
    set(YOAI_ONDEVICE_LLM OFF CACHE BOOL "" FORCE)
    set(YOAI_ASR OFF CACHE BOOL "" FORCE)
endif()

#-------------------------------------------------------------------------------
# Speech output: qwentts.cpp
#-------------------------------------------------------------------------------
#
# Discovered exactly like llama. It also decides WHICH ggml the library links,
# because it ships one of its own and the root CMakeLists refuses to link two.
# That is settled there, not here.
if(NOT DEFINED QTTS_DIR)
    set(_qtts_sdk "${YOAI_PREBUILT_ROOT}/qtts-sdk/${ANDROID_ABI}")
    if(EXISTS "${_qtts_sdk}/include/qwen.h")
        set(QTTS_DIR "${_qtts_sdk}")
    endif()
endif()
if(DEFINED QTTS_DIR)
    set(YOAI_QTTS ON CACHE BOOL "" FORCE)
    message(STATUS "yoai_jni: speech output from ${QTTS_DIR}")
else()
    set(YOAI_QTTS OFF CACHE BOOL "" FORCE)
endif()

add_subdirectory("${YOAI_REPO_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/yoai-core")

# The target is yoai_jni but the artifact must be libyoai.so, to match
# the System.loadLibrary("yoai") on the Kotlin side. Keeping the CMake
# target name distinct also avoids colliding with the core's own target names.
add_library(yoai_jni SHARED jni_init.cpp)
set_target_properties(yoai_jni PROPERTIES OUTPUT_NAME yoai)

target_include_directories(yoai_jni PRIVATE "${YOAI_REPO_ROOT}/src")

# log: __android_log_print, which is where src/utils/log.cpp routes on this
# platform and where the worker threads' output has to land to be readable at
# all -- stderr reaches nobody on a phone.
target_link_libraries(yoai_jni PUBLIC
    log
    android
    yoai_core)
