cmake_minimum_required(VERSION 3.19)
project(bodyclock C CXX)

# The Body Clock core: a static library plus a C ABI (src/bodyclock.h), built to be linked
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

# SQLite, vendored as the amalgamation in third_party/sqlite3 rather than
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
# Costs about 1 MB per ABI inside libbodyclock.so. See third_party/sqlite3/README.md
# for the version and how to move to a new one.
add_library(bodyclock_sqlite3 STATIC third_party/sqlite3/sqlite3.c)
target_include_directories(bodyclock_sqlite3 PUBLIC third_party/sqlite3)

# PUBLIC because several of these change what sqlite3.h declares: a translation
# unit that includes the header must be built with the same set the library was.
target_compile_definitions(bodyclock_sqlite3 PUBLIC
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

# The amalgamation is generated code and warns freely under the core's flags.
if(MSVC)
    target_compile_options(bodyclock_sqlite3 PRIVATE /w)
else()
    target_compile_options(bodyclock_sqlite3 PRIVATE -w)
endif()

# Serialized mode needs pthreads where they are not already implied.
find_package(Threads REQUIRED)
target_link_libraries(bodyclock_sqlite3 PUBLIC Threads::Threads)

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
file(GLOB BODYCLOCK_SQL_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sql/sqlite/*.sql")
list(SORT BODYCLOCK_SQL_FILES)
if(NOT BODYCLOCK_SQL_FILES)
    message(FATAL_ERROR "No migrations found in sql/sqlite/")
endif()

set(_bodyclock_migrations "")
foreach(_sql_file ${BODYCLOCK_SQL_FILES})
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
    string(APPEND _bodyclock_migrations
        "    { ${_sql_version}, \"${_sql_name}\", R\"BODYCLOCK_SQL(\n${_sql_text})BODYCLOCK_SQL\" },\n")
    # Re-run CMake when the statements change, not only when a file is added.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sql_file}")
endforeach()

set(_bodyclock_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${_bodyclock_gen_dir}/store/migrations.hpp.in"
"// Generated by the root CMakeLists.txt from sql/sqlite/*.sql -- do not edit.\n"
"#pragma once\n"
"\n"
"namespace bodyclock {\n"
"namespace store {\n"
"\n"
"struct Migration {\n"
"    int         version;\n"
"    const char* name;\n"
"    const char* sql;\n"
"};\n"
"\n"
"const Migration kMigrations[] = {\n"
"${_bodyclock_migrations}"
"};\n"
"\n"
"const int kMigrationCount = sizeof(kMigrations) / sizeof(kMigrations[0]);\n"
"\n"
"}  // namespace store\n"
"}  // namespace bodyclock\n")
# COPYONLY so an unchanged schema does not touch the header and rebuild the world.
configure_file("${_bodyclock_gen_dir}/store/migrations.hpp.in"
               "${_bodyclock_gen_dir}/store/migrations.hpp" COPYONLY)

#-------------------------------------------------------------------------------
# bodyclock_core
#-------------------------------------------------------------------------------

add_library(bodyclock_core STATIC
    src/client/http_client.cpp
    src/llm/event.cpp
    # Compiles to a stub unless BODYCLOCK_ONDEVICE_LLM is on (see below), so the
    # default build needs no llama.cpp and still exposes the on-device entry
    # points -- they just answer "not built in".
    src/llm/local.cpp
    src/llm/openai_chat.cpp
    src/llm/sse_parser.cpp
    src/platform/c_api.cpp
    src/platform/log.cpp
    src/store/timeline.cpp

    # Voice. One subsystem, two halves with different appetites -- the same split
    # design/voice.md is built on. fbank and prosody are arithmetic over a sample
    # buffer and build everywhere; sensevoice needs ggml and compiles to a
    # truthful stub without it, the way llm/local.cpp does for the LLM lane.
    src/voice/fbank.cpp
    src/voice/prosody.cpp
    src/voice/sensevoice.cpp
)

target_include_directories(bodyclock_core PUBLIC src)
# PRIVATE: the generated migration table is an implementation detail of the
# store, not part of what a host sees.
target_include_directories(bodyclock_core PRIVATE "${_bodyclock_gen_dir}")

if(RapidJSON_FOUND)
    if(TARGET rapidjson)
        target_link_libraries(bodyclock_core PUBLIC rapidjson)
    else()
        target_include_directories(bodyclock_core PUBLIC ${RAPIDJSON_INCLUDE_DIRS})
    endif()
else()
    target_include_directories(bodyclock_core PUBLIC ${RAPIDJSON_INCLUDE_DIR})
endif()

target_link_libraries(bodyclock_core PUBLIC
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    bodyclock_sqlite3
)

# platform/log.cpp routes to the platform's own log sink where there is one --
# stderr reaches nobody on a phone.
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(bodyclock_core PUBLIC log)
elseif(OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    target_link_libraries(bodyclock_core PUBLIC libhilog_ndk.z.so)
endif()

#-------------------------------------------------------------------------------
# ggml, shared by the two on-device lanes
#-------------------------------------------------------------------------------
#
# Two things here run on ggml and they need different amounts of it:
#
#   - The on-device LLM (src/llm/local.cpp) needs libllama on top.
#   - Speech recognition (src/voice/sensevoice.cpp) does NOT. SenseVoice is an
#     encoder plus a CTC head with no autoregression, so it links ggml directly
#     and never touches llama. That is why recognition can be switched on by
#     itself, and why it costs a fraction of what an audio-capable LLM would --
#     see design/voice.md. The rest of src/voice/ needs neither: the filterbank
#     and the prosody measurements are arithmetic over a sample buffer.
#
# Both read the SAME SDK directory, because harmony/build.cmd assembles exactly
# one per ABI and it carries the repo's tuned -march flags. LLAMA_CPP_DIR points
# at it: include/ plus lib/. Both layouts work -- a shared build (one libllama)
# and a static build (libllama plus the ggml archives), which is what a HAP
# needs since there is nowhere to put a companion .so.
#
# On HarmonyOS nobody sets the variable by hand: `harmony/build.cmd <abi> llama`
# (or .sh) assembles the SDK at harmony/prebuilt/llama-sdk/<abi> and the app's
# CMakeLists finds it there. Building the SDK IS turning the lanes on.

# Resolves the ggml archives out of LLAMA_CPP_DIR into BODYCLOCK_GGML_LIBS, and
# fails with the reason rather than with a link error two minutes later.
# `_why` names the option that asked, so the message says which switch to look at.
macro(bodyclock_require_ggml _why)
    if(NOT DEFINED LLAMA_CPP_DIR)
        message(FATAL_ERROR
            "${_why}=ON requires -DLLAMA_CPP_DIR=<llama.cpp SDK dir (include/, lib/)>")
    endif()
    if(NOT EXISTS "${LLAMA_CPP_DIR}/include/ggml.h")
        message(FATAL_ERROR "ggml.h not found under ${LLAMA_CPP_DIR}/include.")
    endif()

    # find_library caches, so without clearing it a changed LLAMA_CPP_DIR
    # (switching target ABI) would silently keep linking the previously found libs.
    foreach(_v GGML_LIB GGML_CPU_LIB GGML_BASE_LIB)
        unset(${_v} CACHE)
    endforeach()

    # NO_CMAKE_FIND_ROOT_PATH is required for the cross build: the OHOS toolchain
    # sets CMAKE_FIND_ROOT_PATH_MODE_LIBRARY to ONLY, which re-roots PATHS under
    # the sysroot and makes an absolute SDK dir unfindable.
    set(_ggml_find PATHS "${LLAMA_CPP_DIR}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_library(GGML_LIB      NAMES ggml      ${_ggml_find})
    find_library(GGML_CPU_LIB  NAMES ggml-cpu  ${_ggml_find})
    find_library(GGML_BASE_LIB NAMES ggml-base ${_ggml_find})

    # A shared llama build puts all of ggml inside libllama, so finding none of
    # these is only an error when nothing else supplies them.
    set(BODYCLOCK_GGML_LIBS "")
    foreach(_lib GGML_LIB GGML_CPU_LIB GGML_BASE_LIB)
        if(${_lib})
            list(APPEND BODYCLOCK_GGML_LIBS "${${_lib}}")
        endif()
    endforeach()
endmacro()

#-------------------------------------------------------------------------------
# On-device LLM (llama.cpp + GGUF)
#-------------------------------------------------------------------------------

# OFF by default: the engine is a few hundred MB of static archives that only the
# mobile hosts want, and src/llm/local.cpp compiles a stub without it, so the
# desktop build stays a two-minute vcpkg affair.
option(BODYCLOCK_ONDEVICE_LLM "Build the on-device LLM engine (llama.cpp + GGUF)" OFF)

if(BODYCLOCK_ONDEVICE_LLM)
    bodyclock_require_ggml(BODYCLOCK_ONDEVICE_LLM)

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

    target_compile_definitions(bodyclock_core PUBLIC BODYCLOCK_ONDEVICE_LLM)
    target_include_directories(bodyclock_core PRIVATE "${LLAMA_CPP_DIR}/include")

    # llama first: with static archives the linker resolves left to right, and
    # llama references ggml, not the other way round.
    target_link_libraries(bodyclock_core PUBLIC "${LLAMA_LIB}")
    target_link_libraries(bodyclock_core PUBLIC ${BODYCLOCK_GGML_LIBS})

    message(STATUS "bodyclock_core: on-device LLM ON -> ${LLAMA_LIB}")
endif()

#-------------------------------------------------------------------------------
# On-device speech recognition (SenseVoice + ggml)
#-------------------------------------------------------------------------------

# OFF by default for the same reason as the LLM lane -- src/voice/sensevoice.cpp
# compiles to a truthful stub without it -- but it is a much smaller ask: ggml
# only, no llama, and a 254 MB model at q8 against several GB for an
# audio-capable LLM.
option(BODYCLOCK_ASR "Build on-device speech recognition (SenseVoice + ggml)" OFF)

if(BODYCLOCK_ASR)
    bodyclock_require_ggml(BODYCLOCK_ASR)
    if(NOT BODYCLOCK_GGML_LIBS AND NOT BODYCLOCK_ONDEVICE_LLM)
        message(FATAL_ERROR
            "No ggml archives under ${LLAMA_CPP_DIR}/lib, and the LLM lane that would "
            "otherwise carry them is off. Recognition links ggml directly and cannot "
            "run without it.")
    endif()

    # PRIVATE: only src/voice/sensevoice.cpp changes shape with this. The public
    # header does not branch on it -- a host asks SenseVoice::available() at
    # runtime, so the same declarations link either way.
    target_compile_definitions(bodyclock_core PRIVATE BODYCLOCK_ASR)
    target_include_directories(bodyclock_core PRIVATE "${LLAMA_CPP_DIR}/include")
    target_link_libraries(bodyclock_core PUBLIC ${BODYCLOCK_GGML_LIBS})

    message(STATUS "bodyclock_core: speech recognition ON -> ${LLAMA_CPP_DIR}")
endif()

#-------------------------------------------------------------------------------
# cli -- a desktop harness for the core
#-------------------------------------------------------------------------------

# Off when cross-compiling: a HAP has nowhere to put a companion executable, and
# the device refuses to exec a standalone binary anyway.
if(CMAKE_CROSSCOMPILING)
    set(_bodyclock_cli_default OFF)
else()
    set(_bodyclock_cli_default ON)
endif()
option(BODYCLOCK_BUILD_CLI "Build the desktop CLI harness (cli/)" ${_bodyclock_cli_default})

if(BODYCLOCK_BUILD_CLI)
    add_executable(bodyclock-cli cli/main.cpp)
    target_link_libraries(bodyclock-cli PRIVATE bodyclock_core)

    # Rides on the same switch as the CLI, and for the same reason: both are
    # desktop harnesses for the core, and a HAP has nowhere to put an
    # executable. The timeline's rules are the one part of the core that fails
    # silently in the data rather than loudly at runtime, so they get a test.
    add_executable(bodyclock-timeline-test tests/timeline_test.cpp)
    target_link_libraries(bodyclock-timeline-test PRIVATE bodyclock_core)

    # Builds whether or not BODYCLOCK_ASR is on. Without it the front-end checks
    # still run and still mean something -- the filterbank is the half that has
    # one right answer -- and the transcription half says it is not compiled in.
    add_executable(bodyclock-sensevoice-test tests/sensevoice_test.cpp)
    target_link_libraries(bodyclock-sensevoice-test PRIVATE bodyclock_core)

    # Needs neither a model nor ggml, so it builds in the plain desktop
    # configuration and its table can be checked against the tool that produced
    # the result design/voice.md records.
    add_executable(bodyclock-prosody-test tests/prosody_test.cpp)
    target_link_libraries(bodyclock-prosody-test PRIVATE bodyclock_core)
endif()
