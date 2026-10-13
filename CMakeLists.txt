cmake_minimum_required(VERSION 3.19)
project(yoai C CXX)

# The YoAI core: a static library plus a C ABI (src/yoai.h), built to be linked
# INTO a host app -- the HarmonyOS native module today, an Android one next --
# rather than run as a process. The core binds no port: the host owns storage
# and settings and reaches the core through the C ABI. The one host that does
# serve is examples/server/, speech over a WebSocket, which links yoai_core
# from a CMake project of its own like the other hosts.
#
# A desktop CLI (cli/) links the same library, so the core can be exercised on a
# workstation without a device in the loop. It is a test harness, not a product.
#
# Dependencies come from vcpkg (see vcpkg.json). Cross-compiling for HarmonyOS
# goes through examples/harmony/build.{sh,cmd}, which lays the same ports out
# under examples/harmony/prebuilt/<abi>/ for the find_package calls below to resolve.

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

# RapidJSON is not resolved here: it is vendored in third/rapidjson and added
# to the target below. See there for why.

# SQLite, vendored as the amalgamation in third/sqlite3 rather than
# resolved like the three above. Two reasons, and neither is about vcpkg:
#
#  - Every mobile target HAS SQLite and none of them LETS C++ LINK IT. The
#    Android NDK has never exposed libsqlite3 as a stable native library, and
#    HarmonyOS's native side offers its own OH_Rdb_* relational store, not the
#    sqlite3 C API. Either way a C++ core has to carry its own copy, which is
#    what Chrome, Signal and SQLCipher all do. iOS is the lone exception.
#  - Since we are carrying a copy regardless, carrying the source means every
#    platform compiles the same bytes from a version pinned by the file itself
#    -- the same argument this build already makes for llama.cpp.
#
# Costs about 1 MB per ABI inside libyoai.so. See third/sqlite3/README.md
# for the version and how to move to a new one.
add_library(yoai_sqlite3 STATIC third/sqlite3/sqlite3.c)
target_include_directories(yoai_sqlite3 PUBLIC third/sqlite3)

# PUBLIC because several of these change what sqlite3.h declares: a translation
# unit that includes the header must be built with the same set the library was.
target_compile_definitions(yoai_sqlite3 PUBLIC
    # The store is reached from the UI thread and from worker threads, so pay
    # for serialized mode rather than police the calls.
    SQLITE_THREADSAFE=1
    # Foreign keys are OFF by default in SQLite, per connection. The schema
    # relies on them, and a default is harder to forget than a pragma.
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    # A double-quoted string is a syntax error instead of silently becoming a
    # string literal, so a mistyped column name fails on the first run rather
    # than returning its own name forever.
    SQLITE_DQS=0
    # Per-allocation bookkeeping nothing here reads.
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_SHARED_CACHE
    # Nothing loads an extension, and on a device this is attack surface.
    SQLITE_OMIT_LOAD_EXTENSION
)

# Vendored C is upstream's code, never edited here, and warns freely under the
# core's flags, so it compiles with warnings off.
#
# On MSVC that means taking the directory's /W3 OUT of the target rather than
# adding /w beside it: cl reports one warning level overriding another as a
# warning of its own (D9025), once per file, which is exactly the noise this is
# meant to remove.
function(yoai_no_warnings target)
    if(MSVC)
        get_target_property(_opts ${target} COMPILE_OPTIONS)
        if(_opts)
            list(REMOVE_ITEM _opts /W3)
            set_target_properties(${target} PROPERTIES COMPILE_OPTIONS "${_opts}")
        endif()
        target_compile_options(${target} PRIVATE /w)
    else()
        target_compile_options(${target} PRIVATE -w)
    endif()
endfunction()

yoai_no_warnings(yoai_sqlite3)

# Serialized mode needs pthreads where they are not already implied.
find_package(Threads REQUIRED)
target_link_libraries(yoai_sqlite3 PUBLIC Threads::Threads)

# sqlite-vec, vendored beside it: vector search as a vec0 virtual table, for
# src/store/vector_index. See third/sqlite-vec/README.md for the version.
#
# Compiled in rather than loaded, so SQLITE_OMIT_LOAD_EXTENSION above stays.
# src/store/db.cpp gives the one connection it by calling sqlite3_vec_init
# directly.
add_library(yoai_sqlite_vec STATIC third/sqlite-vec/sqlite-vec.c)
target_include_directories(yoai_sqlite_vec PUBLIC third/sqlite-vec)
target_link_libraries(yoai_sqlite_vec PUBLIC yoai_sqlite3)

target_compile_definitions(yoai_sqlite_vec
    PUBLIC
        # Built into the library, so it calls the sqlite3 API directly instead of
        # through the routine table a loader hands over. PUBLIC because
        # sqlite-vec.h makes the same choice for whoever includes it: without
        # this it asks for sqlite3ext.h, which is not vendored.
        SQLITE_CORE
        # No __declspec(dllexport) on the entry point: it is linked, not exported.
        SQLITE_VEC_STATIC
    PRIVATE
        # Drops the functions that read .npy files by path. SQL that can open
        # files is the same attack surface OMIT_LOAD_EXTENSION closes.
        SQLITE_VEC_OMIT_FS
)

yoai_no_warnings(yoai_sqlite_vec)
if(NOT MSVC)
    # sqlite-vec.c typedefs uint8_t from u_int8_t on every non-Windows target
    # without including the header that declares it. glibc and bionic declare
    # it anyway; musl -- HarmonyOS -- does not, and the file does not compile.
    target_compile_options(yoai_sqlite_vec PRIVATE -include sys/types.h)
endif()

#-------------------------------------------------------------------------------
# Schema migrations
#-------------------------------------------------------------------------------
#
# sql/sqlite/NNN_name.sql, in order, baked into the binary as a table of
# {version, name, sql}.
#
# They are files so that schema is read and reviewed as SQL, and so that the
# numbered sequence IS the migration order rather than a convention someone has
# to maintain in C++. They are baked in because the core is a static library
# inside a .so inside a HAP: there is no sql/ directory on the device, and
# having the host hand the statements in would put the host back in charge of
# the schema, which is the thing moving storage into the core was meant to end.
#
# Only the schema lives here. Query text stays next to the code that binds its
# parameters and reads its columns -- splitting those two apart is how a column
# order silently stops matching the indices that read it.
file(GLOB YOAI_SQL_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sql/sqlite/*.sql")
list(SORT YOAI_SQL_FILES)
if(NOT YOAI_SQL_FILES)
    message(FATAL_ERROR "No migrations found in sql/sqlite/")
endif()

set(_yoai_migrations "")
foreach(_sql_file ${YOAI_SQL_FILES})
    get_filename_component(_sql_name "${_sql_file}" NAME)
    if(NOT _sql_name MATCHES "^([0-9]+)_.+\\.sql$")
        message(FATAL_ERROR "Migration ${_sql_name} is not named NNN_something.sql")
    endif()
    # Strip leading zeros by hand: math(EXPR) is not the place to discover how a
    # given CMake feels about 007.
    string(REGEX REPLACE "^0+" "" _sql_version "${CMAKE_MATCH_1}")
    if(_sql_version STREQUAL "")
        message(FATAL_ERROR "Migration ${_sql_name} is numbered 0; versions start at 1")
    endif()
    file(READ "${_sql_file}" _sql_text)
    string(APPEND _yoai_migrations
        "    { ${_sql_version}, \"${_sql_name}\", R\"YOAI_SQL(\n${_sql_text})YOAI_SQL\" },\n")
    # Re-run CMake when the statements change, not only when a file is added.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sql_file}")
endforeach()

set(_yoai_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${_yoai_gen_dir}/store/migrations.hpp.in"
"// Generated by the root CMakeLists.txt from sql/sqlite/*.sql -- do not edit.\n"
"#pragma once\n"
"\n"
"namespace yoai {\n"
"namespace store {\n"
"\n"
"struct Migration {\n"
"    int         version;\n"
"    const char* name;\n"
"    const char* sql;\n"
"};\n"
"\n"
"const Migration kMigrations[] = {\n"
"${_yoai_migrations}"
"};\n"
"\n"
"const int kMigrationCount = sizeof(kMigrations) / sizeof(kMigrations[0]);\n"
"\n"
"}  // namespace store\n"
"}  // namespace yoai\n")
# COPYONLY so an unchanged schema does not touch the header and rebuild the world.
configure_file("${_yoai_gen_dir}/store/migrations.hpp.in"
               "${_yoai_gen_dir}/store/migrations.hpp" COPYONLY)

#-------------------------------------------------------------------------------
# yoai_core
#-------------------------------------------------------------------------------

add_library(yoai_core STATIC
    # The C ABI, src/yoai.h. Everything below is reached through it.
    src/yoai.cpp
    # Backend's constructor, which starts each local model's engine. A no-op
    # for an engine the build does not have.
    src/llm/backend.cpp
    # A message's annotations -- its time, how it sounded -- written as the line
    # the model reads in front of it. Every chat client calls it.
    src/llm/chat/annotations.cpp
    src/llm/chat/event.cpp
    # Compiles to a stub unless YOAI_ONDEVICE_LLM is on (see below), so the
    # default build needs no llama.cpp and still exposes the on-device entry
    # points -- they just answer "not built in".
    src/llm/chat/local.cpp
    # The same for MNN: a stub unless YOAI_MNN is on.
    src/llm/chat/mnn.cpp
    src/llm/chat/openai.cpp
    src/llm/chat/sse_parser.cpp
    # What the core and its hosts send to a model, with compiled-in defaults, so
    # no host keeps its own copy. Plain text, no engine.
    src/llm/prompt/prompts.cpp
    # Text to vectors for search by meaning, on the same llama.cpp and the same
    # switch as chat/local.cpp above, and a stub the same way without it.
    src/llm/embed/local.cpp
    # The one connection to yoai.db, and the modules that are views over it.
    src/store/db.cpp
    src/store/timeline.cpp
    src/store/vector_index.cpp
    # Whatever is too small for a subsystem of its own.
    src/utils/bundle.cpp
    src/utils/http.cpp
    src/utils/log.cpp
    # No engine behind it: it reads a buffer and divides. See its header for the
    # claim it is there to check.
    src/utils/memory_bandwidth.cpp

    # Voice. One subsystem, two halves with different appetites -- the same split
    # docs/voice.md is built on. algorithms/, aec/ and prosody are arithmetic
    # over a sample buffer and build everywhere; sensevoice needs ggml and
    # compiles to a truthful stub without it, the way llm/chat/local.cpp does for
    # the LLM lane.
    src/voice/algorithms/fft.cpp
    src/voice/algorithms/fbank_funasr.cpp
    # For the echo canceller's reference signal only; see its header for why
    # the core's no-resampler rule does not cover that.
    src/voice/algorithms/resampler.cpp
    # What the speaker played, taken back out of what the microphone heard.
    src/voice/aec/echo_canceller.cpp
    # How far behind the reference the echo is, from what is being played.
    src/voice/aec/delay_estimator.cpp
    src/voice/asr/sensevoice.cpp
    src/voice/prosody/prosody.cpp
    # A SECOND engine, for the one thing the other two cannot do between them:
    # take audio in and answer in the model's own voice, with no text in the
    # middle. It lost to the third on size, on carrying a second framework and
    # on having no way to ask for a voice -- docs/voice.md has the table --
    # and is back because the end-to-end shape is the thing a chain cannot
    # imitate. Stubs out without YOAI_MNN.
    src/voice/omni/qwen25_omni.cpp
    # A THIRD, for the speaking half alone at a third of the cost, on text
    # somebody else wrote. Stubs out without YOAI_QTTS.
    src/voice/tts/qwen3_tts.cpp
    # And a passage over the top of it: split into sentences, several engines at
    # once, emitted in order. No engine of its own, so it builds everywhere --
    # without YOAI_QTTS its load() fails the way qwen3_tts.cpp's does.
    src/voice/tts/reader.cpp
    # Streamed text cut into pieces that can each be spoken on their own. Plain
    # text processing, no engine; the reader is its first user.
    src/voice/text/sentence_stream.cpp
    # A reply as written -- markdown, emoji -- turned into what is spoken, ahead
    # of the cutter. In the core so no host has to write it again.
    src/voice/text/speakable_stream.cpp
)

target_include_directories(yoai_core PUBLIC src)
# PRIVATE: the generated migration table is an implementation detail of the
# store, not part of what a host sees.
target_include_directories(yoai_core PRIVATE "${_yoai_gen_dir}")

# RapidJSON, vendored: header-only, so a package manager has nothing to build
# for it. See third/rapidjson/README.md for the version.
#
# On the target itself rather than as an interface library, so it comes ahead
# of every directory a linked package brings in. curl's include/ can hold a
# RapidJSON of its own -- a vcpkg tree, or a mobile prebuilt/ from before this
# was vendored -- and two versions in one .so is the crash described under the
# MNN lane below. MNN is the one exception, and goes BEFORE this on purpose.
target_include_directories(yoai_core PUBLIC third/rapidjson/include)

target_link_libraries(yoai_core PUBLIC
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    yoai_sqlite_vec
    yoai_sqlite3
)

# utils/log.cpp routes to the platform's own log sink where there is one --
# stderr reaches nobody on a phone.
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(yoai_core PUBLIC log)
elseif(OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    target_link_libraries(yoai_core PUBLIC libhilog_ndk.z.so)
endif()

#-------------------------------------------------------------------------------
# ggml, shared by the three on-device lanes
#-------------------------------------------------------------------------------
#
# Three things here run on ggml and they need different amounts of it:
#
#   - The on-device LLM (src/llm/chat/local.cpp) needs libllama on top.
#   - Speech recognition (src/voice/asr/sensevoice.cpp) does NOT. SenseVoice is an
#     encoder plus a CTC head with no autoregression, so it links ggml directly
#     and never touches llama. That is why recognition can be switched on by
#     itself, and why it costs a fraction of what an audio-capable LLM would --
#     see docs/voice.md. The rest of src/voice/ needs neither: the filterbank
#     and the prosody measurements are arithmetic over a sample buffer.
#   - Speech OUTPUT (src/voice/tts/qwen3_tts.cpp) needs libqwen-core on top,
#     and that comes with a ggml of its own.
#
# **Exactly one ggml goes into the library**, and that last lane is why this is
# worth a paragraph. The llama SDK and the qwentts SDK each ship
# libggml{,-cpu,-base}.a, built from different commits of two trees that move
# independently. Linking both is the rapidjson mistake again in a bigger form:
# the linker resolves each
# ggml symbol from whichever archive it scans first, so one build's compiled code
# would run against the other's implementation.
#
# Measured rather than guessed, because the answer decides whether the third lane
# is affordable at all:
#
# At llama c390d0a against qwentts's ggml 0572d60:
#
#   - libqwen-core.a needs 134 ggml/gguf symbols. llama's ggml is missing ONE,
#     ggml_prec_set_acc -- the replacement for ggml_mul_mat_set_prec, added
#     between the two commits.
#   - libllama.a needs 287. qwentts's ggml is missing NONE.
#
# So the newer superset the older, and the qwentts SDK's copy is the one this
# macro picks whenever that lane is on. It is built from the same -march flags
# by the same script, so nothing is given up for it.
#
# **That is no longer true, and the pins say so.** Both checkouts moved on
# 2026-09-21 (see third/manifest.json), and re-measuring at llama 335b21f against
# qwentts's ggml e2568d7 inverts it: qwen-core needs its 134 and llama's ggml is
# now missing NONE, while libllama needs 288 and qwentts's ggml is missing ONE,
# ggml_dsv4_hc_pre_gated. Whichever copy is kept, one symbol is unmet; only the
# side that goes short has swapped. The choice below is therefore WRONG for the
# current pins and the desktop addon does not link -- the real fix is one ggml
# for both lanes via qwentts's new GGML_SOURCE_DIR, which the same measurement
# says works. docs/voice.md carries the numbers and the one cost (qwentts's
# ggml fork adds a fused snake kernel for the codec that llama's does not have).
#
# It surfaced exactly as predicted: a missing symbol at link time rather than a
# crash, because C symbols are strong -- a genuine collision is a
# duplicate-definition error, not a silent wrong-layout read.
#
# On HarmonyOS nobody sets these variables by hand: `examples/harmony/build.py <abi>
# llama` and `... qtts` assemble the SDKs under examples/harmony/prebuilt/ and the app's
# CMakeLists finds them there. Building an SDK IS turning its lane on.

# Resolves the ggml archives into YOAI_GGML_LIBS, and fails with the reason
# rather than with a link error two minutes later. `_why` names the option that
# asked, so the message says which switch to look at.
macro(yoai_require_ggml _why)
    # Which SDK's copy, per the paragraph above. The qtts one wins when that
    # lane is on, and it can stand alone: src/voice/tts/qwen3_tts.cpp includes
    # qwen.h and no ggml header, so a build with only that lane needs no llama
    # SDK.
    if(YOAI_QTTS AND DEFINED QTTS_DIR)
        set(_ggml_dir "${QTTS_DIR}/lib")
    elseif(DEFINED LLAMA_CPP_DIR)
        set(_ggml_dir "${LLAMA_CPP_DIR}/lib")
    else()
        message(FATAL_ERROR
            "${_why}=ON requires -DLLAMA_CPP_DIR=<llama.cpp SDK dir (include/, lib/)>")
    endif()

    # The headers, which only the lanes that call ggml directly need -- the LLM
    # through llama.h, recognition through ggml.h. Checked whenever the llama SDK
    # is the one in play so a half-assembled SDK fails here and not at compile.
    if(DEFINED LLAMA_CPP_DIR AND NOT EXISTS "${LLAMA_CPP_DIR}/include/ggml.h")
        message(FATAL_ERROR "ggml.h not found under ${LLAMA_CPP_DIR}/include.")
    endif()

    # find_library caches, so without clearing it a changed directory (switching
    # target ABI, or the qtts lane appearing) would silently keep linking the
    # previously found libs.
    foreach(_v GGML_LIB GGML_CPU_LIB GGML_BASE_LIB GGML_VULKAN_LIB GGML_CUDA_LIB
               GGML_METAL_LIB GGML_BLAS_LIB)
        unset(${_v} CACHE)
    endforeach()

    # NO_CMAKE_FIND_ROOT_PATH is required for the cross build: the OHOS toolchain
    # sets CMAKE_FIND_ROOT_PATH_MODE_LIBRARY to ONLY, which re-roots PATHS under
    # the sysroot and makes an absolute SDK dir unfindable.
    set(_ggml_find PATHS "${_ggml_dir}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_library(GGML_LIB        NAMES ggml        ${_ggml_find})
    find_library(GGML_CPU_LIB    NAMES ggml-cpu    ${_ggml_find})
    find_library(GGML_BASE_LIB   NAMES ggml-base   ${_ggml_find})
    # Optional, and present only when the SDK was built with `vulkan`. Found by
    # name like the others rather than inferred from a switch, so the archive
    # that exists is the one that gets linked -- the SDK's contents are the
    # record of how it was built. CUDA the same way; the root build.py puts it
    # beside Vulkan wherever there is a CUDA Toolkit.
    find_library(GGML_VULKAN_LIB NAMES ggml-vulkan ${_ggml_find})
    find_library(GGML_CUDA_LIB   NAMES ggml-cuda   ${_ggml_find})
    # Metal and BLAS on macOS, where ggml turns both on by itself for APPLE and
    # libggml's registry names them. Leaving them out is a link error for
    # ggml_backend_metal_reg and ggml_backend_blas_reg.
    find_library(GGML_METAL_LIB  NAMES ggml-metal  ${_ggml_find})
    find_library(GGML_BLAS_LIB   NAMES ggml-blas   ${_ggml_find})

    # A shared llama build puts all of ggml inside libllama, so finding none of
    # these is only an error when nothing else supplies them.
    #
    # The order is the reference order, because with static archives a
    # single-pass linker (GNU ld) resolves left to right: libggml is the backend
    # registry and names every backend compiled in, each backend names
    # ggml-base, and so does the CPU one. lld and MSVC's link.exe resolve in any
    # order, which is why the order this replaced, Vulkan first, linked on
    # Windows, Android and OHOS.
    set(YOAI_GGML_LIBS "")
    foreach(_lib GGML_LIB GGML_METAL_LIB GGML_BLAS_LIB GGML_VULKAN_LIB GGML_CUDA_LIB
                 GGML_CPU_LIB GGML_BASE_LIB)
        if(${_lib})
            list(APPEND YOAI_GGML_LIBS "${${_lib}}")
        endif()
    endforeach()

    # The frameworks those archives need and do not carry, as for CUDA's
    # runtime below. Accelerate for ggml-cpu and ggml-blas, which use it for
    # BLAS and vDSP; the other three only when the Metal backend is in.
    if(APPLE AND YOAI_GGML_LIBS)
        list(APPEND YOAI_GGML_LIBS "-framework Accelerate")
        if(GGML_METAL_LIB)
            list(APPEND YOAI_GGML_LIBS
                 "-framework Foundation" "-framework Metal" "-framework MetalKit")
            message(STATUS "yoai_core: ggml Metal backend ON -> ${GGML_METAL_LIB}")
        endif()
    endif()

    # The loader, which belongs to the platform and not to the SDK. Only when
    # the backend is actually in, so a CPU-only build does not pull a graphics
    # dependency into a library that has no use for one.
    #
    # Two names and a hint, because neither the file name nor its location is
    # the same twice. The import library is `vulkan-1.lib` on Windows and
    # `libvulkan.so` on OHOS and Linux; on OHOS it is in the sysroot, which the
    # toolchain already searches, and on Windows it is under the Vulkan SDK,
    # which nothing searches unless told. VULKAN_SDK is the variable that SDK
    # sets, and the same one ggml's own CMake reads.
    if(GGML_VULKAN_LIB)
        unset(VULKAN_LOADER_LIB CACHE)
        find_library(VULKAN_LOADER_LIB NAMES vulkan-1 vulkan
                     HINTS "$ENV{VULKAN_SDK}/Lib" "$ENV{VULKAN_SDK}/lib")
        if(NOT VULKAN_LOADER_LIB)
            message(FATAL_ERROR
                "ggml's Vulkan backend is in ${GGML_VULKAN_LIB} but no Vulkan "
                "loader (vulkan-1 or vulkan) was found to link it against. "
                "On Windows, set VULKAN_SDK to a Vulkan SDK install.")
        endif()
        list(APPEND YOAI_GGML_LIBS "${VULKAN_LOADER_LIB}")
        # So src/voice/tts/qwen3_tts.cpp can set the backend's environment before
        # the engine initialises it. There is no runtime way to ask ggml whether
        # the Vulkan backend was compiled in early enough to matter.
        target_compile_definitions(yoai_core PRIVATE YOAI_QTTS_VULKAN)
        # And the C header, so that same file can ask Vulkan what KIND of GPU
        # this is before ggml creates one -- the tuning it sets is for a GPU
        # that shares system memory and costs a discrete card dearly. Only the C
        # header is wanted: OHOS's sysroot ships that one and not the C++
        # bindings. When it is not found the probe compiles out and the tuning
        # is applied unconditionally, which is what this did before it existed.
        unset(VULKAN_HEADER_DIR CACHE)
        find_path(VULKAN_HEADER_DIR vulkan/vulkan.h
                  HINTS "$ENV{VULKAN_SDK}/Include" "$ENV{VULKAN_SDK}/include")
        if(VULKAN_HEADER_DIR)
            target_include_directories(yoai_core PRIVATE "${VULKAN_HEADER_DIR}")
            target_compile_definitions(yoai_core PRIVATE YOAI_QTTS_VULKAN_PROBE)
            message(STATUS "yoai_core: Vulkan device probe ON -> ${VULKAN_HEADER_DIR}")
        else()
            message(WARNING
                "yoai_core: no vulkan/vulkan.h found, so the discrete-GPU probe is off "
                "and the integrated-GPU tuning in src/voice/tts/qwen3_tts.cpp applies "
                "everywhere.")
        endif()
        message(STATUS "yoai_core: ggml Vulkan backend ON -> ${GGML_VULKAN_LIB}")
    endif()

    # CUDA's runtime, which the archive needs and does not carry: ggml-cuda
    # links cudart, cuBLAS and the driver PRIVATE, so a link that takes the bare
    # archive has to name them itself. The shared ones, as ggml's own build
    # does unless GGML_STATIC -- the toolkit has no static cuBLAS on Windows.
    # By file rather than as CUDA:: targets, like everything else on this line:
    # an imported target is visible only in this directory, and examples/android/ and
    # examples/harmony/ link yoai_core from theirs. build.py passes CUDAToolkit_ROOT, the
    # toolkit that compiled the archive.
    if(GGML_CUDA_LIB)
        find_package(CUDAToolkit REQUIRED)
        foreach(_cuda cudart cublas cuda_driver)
            if(NOT CUDA_${_cuda}_LIBRARY)
                message(FATAL_ERROR
                    "ggml's CUDA backend is in ${GGML_CUDA_LIB} but the CUDA Toolkit "
                    "${CUDAToolkit_VERSION} at ${CUDAToolkit_LIBRARY_DIR} has no "
                    "${_cuda} library to link it against.")
            endif()
            list(APPEND YOAI_GGML_LIBS "${CUDA_${_cuda}_LIBRARY}")
        endforeach()
        message(STATUS "yoai_core: ggml CUDA backend ON -> ${GGML_CUDA_LIB}, "
                       "CUDA ${CUDAToolkit_VERSION}")
    endif()

    # OpenMP's runtime, which the archives need and do not carry, for the same
    # reason as CUDA's: ggml-cpu links it PRIVATE. ggml turns OpenMP on exactly
    # when find_package(OpenMP) succeeds, and the SDKs of a host build were
    # compiled by this compiler on this machine, so the same question gets the
    # same answer here -- GCC on Linux says yes, Apple's clang says no. MSVC
    # needs nothing: /openmp names vcomp in the objects, and the linker takes
    # it from there. Not for a cross build: examples/android/ and
    # examples/harmony/ build their ggml with GGML_OPENMP=OFF, and a runtime
    # named on their link line would be one more library the app has to ship.
    # After the archives, as a single-pass linker needs.
    if(NOT MSVC AND NOT CMAKE_CROSSCOMPILING)
        find_package(OpenMP QUIET COMPONENTS C)
        if(OpenMP_C_FOUND)
            if(OpenMP_C_LIBRARIES)
                set(_yoai_omp ${OpenMP_C_LIBRARIES})
            else()
                set(_yoai_omp ${OpenMP_C_FLAGS})
            endif()
            list(APPEND YOAI_GGML_LIBS ${_yoai_omp})
            message(STATUS "yoai_core: ggml's OpenMP runtime -> ${_yoai_omp}")
        endif()
    endif()
endmacro()

#-------------------------------------------------------------------------------
# Speech output (qwentts.cpp + Qwen3-TTS VoiceDesign)
#-------------------------------------------------------------------------------

# A third engine, and the cheapest of the three: one static archive on top of the
# ggml already here, against MNN's whole inference framework. It speaks text the
# LLM lane produced, in a voice described in English rather than cloned, and it
# measured 1.16x against omni's 3.0x for the same job on a desktop. See
# src/voice/tts/qwen3_tts.hpp for what the description buys and docs/voice.md
# for the five checkpoints that were tried before this one.
#
# **Declared BEFORE the LLM lane on purpose**, and the reason is the link line
# rather than taste. Static archives resolve left to right, so libqwen-core.a has
# to appear ahead of the ggml archives that satisfy it; putting this section
# after the llama one would append it behind them and every ggml symbol it needs
# would come back undefined.
#
# OFF by default like the others. QTTS_DIR points at an SDK dir shaped like the
# llama one: include/ + lib/. examples/harmony/build.py qtts assembles it at
# examples/harmony/prebuilt/qtts-sdk/<abi>.
option(YOAI_QTTS "Build speech output (qwentts.cpp + Qwen3-TTS)" OFF)

if(YOAI_QTTS)
    if(NOT DEFINED QTTS_DIR)
        message(FATAL_ERROR
            "YOAI_QTTS=ON requires -DQTTS_DIR=<qwentts SDK dir (include/, lib/)>")
    endif()
    if(NOT EXISTS "${QTTS_DIR}/include/qwen.h")
        message(FATAL_ERROR "qwen.h not found under ${QTTS_DIR}/include.")
    endif()

    unset(QWEN_CORE_LIB CACHE)
    find_library(QWEN_CORE_LIB NAMES qwen-core
        PATHS "${QTTS_DIR}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    if(NOT QWEN_CORE_LIB)
        message(FATAL_ERROR "no qwen-core library under ${QTTS_DIR}/lib.")
    endif()

    # The ggml that goes with it -- resolved here rather than left to whichever
    # lane happens to ask first, because this lane is the one that decides which
    # copy wins and the decision has to be made before anything links.
    yoai_require_ggml(YOAI_QTTS)

    # QWEN_STATIC matters only on Windows, and only because qwen.h defaults the
    # other way. Its QT_API expands to __declspec(dllimport) unless told
    # otherwise, so without this every qt_* call compiles to an __imp_ reference
    # that a static qwen-core.lib cannot satisfy -- fifteen unresolved externals
    # naming functions that are plainly present in the archive. The SDK is built
    # with QWEN_SHARED=OFF, so static is what it always is here.
    target_compile_definitions(yoai_core PRIVATE YOAI_QTTS QWEN_STATIC)
    target_include_directories(yoai_core PRIVATE "${QTTS_DIR}/include")
    target_link_libraries(yoai_core PUBLIC "${QWEN_CORE_LIB}")

    message(STATUS "yoai_core: speech output ON -> ${QWEN_CORE_LIB}")
endif()

#-------------------------------------------------------------------------------
# On-device LLM (llama.cpp + GGUF)
#-------------------------------------------------------------------------------

# OFF by default: the engine is a few hundred MB of static archives that only the
# mobile hosts want, and src/llm/chat/local.cpp compiles a stub without it, so the
# desktop build stays a two-minute vcpkg affair.
option(YOAI_ONDEVICE_LLM "Build the on-device LLM engine (llama.cpp + GGUF)" OFF)

if(YOAI_ONDEVICE_LLM)
    yoai_require_ggml(YOAI_ONDEVICE_LLM)

    unset(LLAMA_LIB CACHE)
    find_library(LLAMA_LIB NAMES llama
        PATHS "${LLAMA_CPP_DIR}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

    if(NOT LLAMA_LIB)
        message(FATAL_ERROR
            "llama library not found under ${LLAMA_CPP_DIR}/lib. Check LLAMA_CPP_DIR.")
    endif()
    if(NOT EXISTS "${LLAMA_CPP_DIR}/include/llama.h")
        message(FATAL_ERROR "llama.h not found under ${LLAMA_CPP_DIR}/include.")
    endif()

    # The SDK's API LEVEL, not just its presence. src/llm/chat/local.cpp passes the
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
            "llama_sampler_init_penalties has no n_vocab parameter, so src/llm/chat/local.cpp "
            "will not compile against it.\n"
            "Update the llama.cpp checkout and re-run examples/harmony/build.py <abi> llama "
            "with `clean`, so the cached SDK is discarded rather than reused.")
    endif()
    unset(_llama_h)
    unset(_llama_h_flat)

    target_compile_definitions(yoai_core PUBLIC YOAI_ONDEVICE_LLM)
    target_include_directories(yoai_core PRIVATE "${LLAMA_CPP_DIR}/include")

    # llama only. ggml goes on the end of the link line for every lane at once,
    # below -- with static archives the linker resolves left to right, and llama
    # references ggml, not the other way round.
    target_link_libraries(yoai_core PUBLIC "${LLAMA_LIB}")

    message(STATUS "yoai_core: on-device LLM ON -> ${LLAMA_LIB}")
endif()

#-------------------------------------------------------------------------------
# On-device speech recognition (SenseVoice + ggml)
#-------------------------------------------------------------------------------

# OFF by default for the same reason as the LLM lane --
# src/voice/asr/sensevoice.cpp compiles to a truthful stub without it -- but it
# is a much smaller ask: ggml only, no llama, and a 254 MB model at q8 against
# several GB for an audio-capable LLM.
option(YOAI_ASR "Build on-device speech recognition (SenseVoice + ggml)" OFF)

if(YOAI_ASR)
    yoai_require_ggml(YOAI_ASR)
    if(NOT YOAI_GGML_LIBS AND NOT YOAI_ONDEVICE_LLM)
        message(FATAL_ERROR
            "No ggml archives under ${LLAMA_CPP_DIR}/lib, and the LLM lane that would "
            "otherwise carry them is off. Recognition links ggml directly and cannot "
            "run without it.")
    endif()

    # PRIVATE: only src/voice/asr/sensevoice.cpp changes shape with this. The
    # public header does not branch on it -- a host asks SenseVoice::available()
    # at runtime, so the same declarations link either way.
    target_compile_definitions(yoai_core PRIVATE YOAI_ASR)
    target_include_directories(yoai_core PRIVATE "${LLAMA_CPP_DIR}/include")

    message(STATUS "yoai_core: speech recognition ON -> ${LLAMA_CPP_DIR}")
endif()

# ggml LAST, and once for all three lanes.
#
# Static archives resolve left to right, so every archive that references ggml --
# libqwen-core.a, libllama.a and our own sensevoice objects -- has to be ahead of
# it on the line. Each lane used to append this itself, which worked only because
# the two lanes that did were adjacent; a third one declared earlier would have
# pulled ggml up in front of llama and left llama's every ggml symbol undefined.
# One line here cannot get that wrong.
if(YOAI_GGML_LIBS)
    target_link_libraries(yoai_core PUBLIC ${YOAI_GGML_LIBS})
endif()

#-------------------------------------------------------------------------------
# The MNN engine: omni speech output, and chat
#-------------------------------------------------------------------------------

# A second inference engine, carried for two things.
#
#   - Omni speech output. MNN has the omni Talker -- thinker to codec tokens to
#     a diffusion transformer to a vocoder -- and llama.cpp has no Talker
#     checkpoint in GGUF at all. src/voice/omni.* is deliberately narrow: no
#     transcription, no scoring.
#   - Chat on an MNN export (src/llm/chat/mnn.cpp), as the second local engine
#     beside llama.cpp. A build can have either or both; yoai_local_set picks
#     by the model path. Worth having where MNN's GPU backends beat llama.cpp's
#     CPU-only build, which is unmeasured.
#
# OFF by default, and more firmly than the other lanes. It is a second model
# format and a second quantization story, and the omni half is 3.8 GB of
# weights bought for one capability whose cost has not been measured on a phone
# -- about 3x slower than real time on a desktop CPU. See docs/voice.md
# before turning it on.
#
# MNN_DIR points at an SDK dir shaped like the llama one: include/ + lib/.
# examples/harmony/build.py mnn assembles it at examples/harmony/prebuilt/mnn-sdk/<abi>.
option(YOAI_MNN "Build the MNN engine: omni speech output and chat" OFF)

if(YOAI_MNN)
    if(NOT DEFINED MNN_DIR)
        message(FATAL_ERROR
            "YOAI_MNN=ON requires -DMNN_DIR=<MNN SDK dir (include/, lib/)>")
    endif()
    if(NOT EXISTS "${MNN_DIR}/include/llm/llm.hpp")
        message(FATAL_ERROR "llm/llm.hpp not found under ${MNN_DIR}/include.")
    endif()
    # Not optional, and not a convenience. See the include-order note below.
    if(NOT EXISTS "${MNN_DIR}/include/rapidjson/document.h")
        message(FATAL_ERROR
            "no rapidjson under ${MNN_DIR}/include. The MNN SDK must ship the "
            "rapidjson libMNN was compiled with -- two versions in one .so "
            "crash on the first JSON parse. Re-run examples/harmony/build.py mnn.")
    endif()

    unset(MNN_LIB CACHE)
    find_library(MNN_LIB NAMES MNN
        PATHS "${MNN_DIR}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    if(NOT MNN_LIB)
        message(FATAL_ERROR "no MNN library under ${MNN_DIR}/lib.")
    endif()

    # ONE archive by design: built with MNN_SEP_BUILD=OFF, the llm engine is an
    # OBJECT library folded into libMNN, so there is no libllm to find. Looking
    # for one reports a good build as a failure.
    target_compile_definitions(yoai_core PRIVATE YOAI_MNN)

    # Whether that libMNN was built with -DMNN_VULKAN=ON, which the archive
    # itself cannot be asked.
    #
    # It matters because MNN's Vulkan backend does not survive static linking on
    # its own. EVERY part of it registers from a file-scope initializer -- the
    # runtime creator in VulkanRuntime.cpp, and one addCreator per operator in
    # its own file -- and an initializer only runs if the linker pulls its
    # object out of the archive. Nothing else in MNN references any of them.
    #
    # The failures are separate and look nothing alike. Without the runtime
    # creator the backend does not exist at all: "Can't Find type=7 backend, use
    # 0 instead", and everything quietly runs on the CPU. WITH it and without
    # the operators, the backend is chosen and then cannot execute most of the
    # graph: a page of "Vulkan don't support 7, BinaryOp" and a fallback to the
    # CPU per operator, which measured FIVE TIMES SLOWER than not using the GPU
    # at all -- 14.4 s of token2wav against 3.1 s, plus 15.6 s of copying that
    # no counter attributes to anything.
    #
    # So the whole archive goes in rather than one referenced symbol. It is
    # blunt and it costs binary size; the alternative is naming every operator
    # file MNN might ever add.
    option(YOAI_MNN_VULKAN "The MNN at MNN_DIR has the Vulkan backend" OFF)
    if(YOAI_MNN_VULKAN)
        target_compile_definitions(yoai_core PRIVATE YOAI_MNN_VULKAN)
        if(MSVC)
            # INTERFACE: this is a static library, so the option has to reach
            # whoever produces the binary -- the addon, or the CLI.
            target_link_options(yoai_core INTERFACE "/WHOLEARCHIVE:${MNN_LIB}")
        else()
            target_link_options(yoai_core INTERFACE
                "-Wl,--whole-archive" "${MNN_LIB}" "-Wl,--no-whole-archive")
        endif()
    endif()

    # BEFORE, and that word is the whole fix for a crash that looked like a
    # memory problem for a day.
    #
    # rapidjson is header-only templates: its member functions are emitted as
    # weak symbols into every object that includes it, and the linker keeps ONE
    # definition of each across the .so. src/llm and src/utils include the
    # rapidjson this repo vendors in third/rapidjson (a 2025 master); libMNN.a was
    # compiled against the 1.1.0 release MNN keeps in 3rd_party. Same mangled
    # names, different struct layouts -- the newer MemoryPoolAllocator holds a
    # SharedData* where 1.1.0 holds chunkHead_ -- so one copy wins the link and
    # then runs against the other's objects.
    #
    # Measured: SIGSEGV(SEGV_MAPERR)@0x10 in MemoryPoolAllocator::Malloc, from
    # LlmConfig's first parse inside Llm::createLLM, with no MNN diagnostic
    # ahead of it because MNN had not reached any code of its own yet.
    #
    # So this directory goes ahead of the vendored one and OUR two JSON files
    # compile against MNN's copy as well. They are the only rapidjson users in
    # the library; if that changes, the new one has to be in this target too,
    # or the same crash comes back somewhere else.
    target_include_directories(yoai_core BEFORE PRIVATE "${MNN_DIR}/include")
    target_link_libraries(yoai_core PUBLIC "${MNN_LIB}")

    message(STATUS "yoai_core: MNN (omni, chat) ON -> ${MNN_LIB}")
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

    # Recognition from the command line: a model, then WAV files. Builds
    # whether or not YOAI_ASR is on and says which it is, the same way the
    # engine does. tests/ for wav.hpp, the one WAV reader the harnesses share.
    add_executable(yoai-asr cli/asr.cpp)
    target_link_libraries(yoai-asr PRIVATE yoai_core)
    target_include_directories(yoai-asr PRIVATE tests)

    # A GGUF from the command line: chat or embed. Builds whether or not
    # YOAI_ONDEVICE_LLM is on and says which. shell32 for CommandLineToArgvW,
    # which cli/console.hpp uses to read the arguments as UTF-8.
    add_executable(yoai-llama cli/llama.cpp)
    target_link_libraries(yoai-llama PRIVATE yoai_core)
    if(WIN32)
        target_link_libraries(yoai-llama PRIVATE shell32)
    endif()

    # Rides on the same switch as the CLI, and for the same reason: both are
    # desktop harnesses for the core, and a HAP has nowhere to put an
    # executable. The timeline's rules are the one part of the core that fails
    # silently in the data rather than loudly at runtime, so they get a test.
    add_executable(yoai-timeline-test tests/store/timeline_test.cpp)
    target_link_libraries(yoai-timeline-test PRIVATE yoai_core)

    # The same store through the door a HOST uses, which is a separate thing to
    # get wrong: the rules can be perfect while a JSON key is spelled
    # differently than yoai.h says, and a host only ever reads the header.
    add_executable(yoai-timeline-abi-test tests/store/timeline_abi_test.cpp)
    target_link_libraries(yoai-timeline-abi-test PRIVATE yoai_core)

    # Vector search over an in-memory database. Checks that sqlite-vec is
    # compiled in and registers, and that a nearest-neighbor query returns
    # what was stored, in distance order.
    add_executable(yoai-vector-index-test tests/store/vector_index_test.cpp)
    target_link_libraries(yoai-vector-index-test PRIVATE yoai_core)

    # The embedder and the index together: text in, the related text out.
    # Builds whether or not YOAI_ONDEVICE_LLM is on, and says which.
    add_executable(yoai-embed-test tests/llm/embed_test.cpp)
    target_link_libraries(yoai-embed-test PRIVATE yoai_core)

    # Builds whether or not YOAI_ASR is on. Without it the front-end checks
    # still run and still mean something -- the filterbank is the half that has
    # one right answer -- and the transcription half says it is not compiled in.
    add_executable(yoai-sensevoice-test tests/voice/asr/sensevoice_test.cpp)
    target_link_libraries(yoai-sensevoice-test PRIVATE yoai_core)
    target_include_directories(yoai-sensevoice-test PRIVATE tests)

    # Needs neither a model nor ggml, so it builds in the plain desktop
    # configuration and its table can be checked against the tool that produced
    # the result docs/voice.md records.
    add_executable(yoai-prosody-test tests/voice/prosody/prosody_test.cpp)
    target_link_libraries(yoai-prosody-test PRIVATE yoai_core)
    target_include_directories(yoai-prosody-test PRIVATE tests)

    # The resampler alone: closed-form checks with no arguments, a WAV
    # converted to 16 kHz with two, to be listened to against the original.
    add_executable(yoai-resampler-test tests/voice/algorithms/resampler_test.cpp)
    target_link_libraries(yoai-resampler-test PRIVATE yoai_core)
    target_include_directories(yoai-resampler-test PRIVATE tests)

    # The echo canceller against synthetic echo whose every part is known, so
    # the residual can be measured exactly, even while someone talks over it.
    add_executable(yoai-echo-canceller-test tests/voice/aec/echo_canceller_test.cpp)
    target_link_libraries(yoai-echo-canceller-test PRIVATE yoai_core)
    target_include_directories(yoai-echo-canceller-test PRIVATE tests)

    # The same canceller through the door a host uses: its reference resampled,
    # the two streams paired however they arrive, against the class run directly.
    add_executable(yoai-aec-abi-test tests/voice/aec/aec_abi_test.cpp)
    target_link_libraries(yoai-aec-abi-test PRIVATE yoai_core)

    # The delay estimator against delays it was not told, through synthetic
    # rooms; with two WAVs, a window-by-window trace of a real pair.
    add_executable(yoai-delay-estimator-test tests/voice/aec/delay_estimator_test.cpp)
    target_link_libraries(yoai-delay-estimator-test PRIVATE yoai_core)
    target_include_directories(yoai-delay-estimator-test PRIVATE tests)

    # One file in, a directory out. Checked against tools/pack-mnn-model.py:
    # a truncated weight file is the one failure MNN would answer through
    # rather than report.
    add_executable(yoai-bundle-test tests/utils/bundle_test.cpp)
    target_link_libraries(yoai-bundle-test PRIVATE yoai_core)

    # The sentence splitter, which needs no engine. It has a test because its
    # predecessor failed in the one way nothing reports: the reply was read
    # aloud and simply did not say everything.
    add_executable(yoai-reader-test tests/voice/tts/reader_test.cpp)
    target_link_libraries(yoai-reader-test PRIVATE yoai_core)

    # The prompt table, through the class and through the C ABI a host reads.
    # With --dump it prints yoai_prompts() instead, to see what a host gets.
    add_executable(yoai-prompts-test tests/llm/prompts_test.cpp)
    target_link_libraries(yoai-prompts-test PRIVATE yoai_core)

    # When a message's time and arousal reach the model, and that adding a turn
    # never rewrites an earlier one -- the property every cached prefix rests on.
    add_executable(yoai-annotations-test tests/llm/annotations_test.cpp)
    target_link_libraries(yoai-annotations-test PRIVATE yoai_core)

    # One passage at several talker temperatures and seeds, written out to be
    # listened to. Needs the engine to do anything; builds everywhere.
    add_executable(yoai-tts-sweep tests/voice/tts/tts_sweep.cpp)
    target_link_libraries(yoai-tts-sweep PRIVATE yoai_core)

    # Whether a CustomVoice reading style reaches the Base checkpoint, through a
    # voice file or a reference in the prompt. Needs the engine; builds everywhere.
    add_executable(yoai-tts-style tests/voice/tts/tts_style.cpp)
    target_link_libraries(yoai-tts-style PRIVATE yoai_core)

    # A passage read while it is still being "written" at a fixed rate,
    # against the same passage given whole: the audio must match, and the
    # playback it would feed must not run dry.
    add_executable(yoai-tts-pull tests/voice/tts/tts_pull.cpp)
    target_link_libraries(yoai-tts-pull PRIVATE yoai_core)
endif()

#-------------------------------------------------------------------------------
# yoai -- the C ABI as a shared library
#-------------------------------------------------------------------------------

# yoai_core linked into one yoai.dll / libyoai.so / libyoai.dylib, for a host
# that loads the core at runtime instead of linking it in: examples/python/
# reaches it through ctypes. Off when cross-compiling, like the CLI -- the phone
# hosts link yoai_core into a module of their own -- and off in the desktop
# addon's tree, which turns it off before reading this file.
option(YOAI_BUILD_SHARED "Build the C ABI as a shared library (examples/python/)"
       ${_yoai_cli_default})

if(YOAI_BUILD_SHARED)
    # What it exports is read from src/yoai.h, so a function added there is
    # exported without anyone remembering to. Every declaration starts at
    # column 0 and names yoai_<name>( on its first line; a typedef's name is
    # followed by `)`, so the callback types do not match. file(STRINGS) splits
    # a line at each `;`, which leaves pieces with no name in them; those are
    # skipped.
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/yoai.h" _yoai_decls
         REGEX "^[A-Za-z].*[ *]yoai_[a-z0-9_]+\\(")
    set(_yoai_exports "")
    foreach(_line ${_yoai_decls})
        string(REGEX MATCH "yoai_[a-z0-9_]+\\(" _name "${_line}")
        if(_name)
            string(REGEX REPLACE "\\($" "" _name "${_name}")
            list(APPEND _yoai_exports ${_name})
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _yoai_exports)
    if(NOT _yoai_exports)
        message(FATAL_ERROR "No functions found in src/yoai.h to export.")
    endif()

    # Four files from the one list. The C file references every function,
    # because a linker takes from a static archive only what something already
    # in the link needs, and nothing else here needs yoai_core at all. The
    # other three say what the library exports, one per linker: everything else
    # stays inside it -- llama.cpp's and ggml's symbols included, so a process
    # that loads another ggml, as a Python process may, gets no clash.
    set(_refs "")
    set(_def "EXPORTS\n")
    set(_map "{\n  global:\n")
    set(_exp "")
    foreach(_name ${_yoai_exports})
        string(APPEND _refs "    (yoai_fn)${_name},\n")
        string(APPEND _def "    ${_name}\n")
        string(APPEND _map "    ${_name};\n")
        # Mach-O prefixes every C symbol with an underscore.
        string(APPEND _exp "_${_name}\n")
    endforeach()
    string(APPEND _map "  local: *;\n};\n")

    set(_shared_gen "${_yoai_gen_dir}/shared")
    file(WRITE "${_shared_gen}/yoai_shared.c.in"
        "/* Generated by the root CMakeLists.txt from src/yoai.h -- do not edit. */\n"
        "#include \"yoai.h\"\n"
        "\n"
        "typedef void (*yoai_fn)(void);\n"
        "\n"
        "const yoai_fn yoai_shared_exports[] = {\n"
        "${_refs}"
        "};\n")
    file(WRITE "${_shared_gen}/yoai.def.in" "${_def}")
    file(WRITE "${_shared_gen}/yoai.map.in" "${_map}")
    file(WRITE "${_shared_gen}/yoai.exp.in" "${_exp}")
    # COPYONLY, as for the migrations: an unchanged header relinks nothing.
    foreach(_f yoai_shared.c yoai.def yoai.map yoai.exp)
        configure_file("${_shared_gen}/${_f}.in" "${_shared_gen}/${_f}" COPYONLY)
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/src/yoai.h")

    add_library(yoai_shared SHARED "${_shared_gen}/yoai_shared.c")
    set_target_properties(yoai_shared PROPERTIES OUTPUT_NAME yoai)
    target_link_libraries(yoai_shared PRIVATE yoai_core)
    if(WIN32)
        target_sources(yoai_shared PRIVATE "${_shared_gen}/yoai.def")
    elseif(APPLE)
        target_link_options(yoai_shared PRIVATE
            "LINKER:-exported_symbols_list,${_shared_gen}/yoai.exp")
    else()
        target_link_options(yoai_shared PRIVATE
            "LINKER:--version-script=${_shared_gen}/yoai.map")
        # The libraries it loads, looked for beside it first, as Windows does
        # for a DLL's.
        set_target_properties(yoai_shared PROPERTIES BUILD_RPATH "$ORIGIN")
    endif()

    list(LENGTH _yoai_exports _yoai_export_count)
    message(STATUS "yoai: shared library ON, ${_yoai_export_count} functions exported")
endif()
