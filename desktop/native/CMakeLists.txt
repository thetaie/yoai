cmake_minimum_required(VERSION 3.19)
project(bodyclock_addon CXX)

# The Node-API addon: bodyclock_core, plus src/binding.cpp, linked into one
# bodyclock.node that Electron's MAIN PROCESS requires.
#
# It is the desktop equivalent of harmony/entry/src/main/cpp -- same core, same
# C ABI, a different host's module format -- and it links the repo's root
# CMakeLists rather than copying any of it, so a change to the core reaches the
# desktop client by rebuilding and nothing else.
#
# Driven by cmake-js, which supplies CMAKE_JS_INC (the Node headers), CMAKE_JS_SRC
# (the Windows delay-load hook) and CMAKE_JS_LIB, and passes everything else
# through from ../build.{cmd,sh}: the vcpkg toolchain that resolves curl, OpenSSL
# and RapidJSON, and the on-device lane's -DBODYCLOCK_ONDEVICE_LLM / -DLLAMA_CPP_DIR.

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Node-API 8 is the floor: threadsafe functions arrived in 4, and 8 is what every
# Electron this project would run on already carries. Declaring it pins the
# surface the addon is allowed to use, so a call from a newer API level fails to
# compile here rather than failing to load on someone else's machine.
add_definitions(-DNAPI_VERSION=8)

#-------------------------------------------------------------------------------
# The core
#-------------------------------------------------------------------------------
#
# The root build defaults BODYCLOCK_BUILD_CLI to ON off-device, which would build
# the CLI harness and four test executables every time the addon is rebuilt.
# Nothing here runs them, so they are turned off before the subdirectory is read
# -- FORCE because they are cached options and a stale ON would survive.
set(BODYCLOCK_BUILD_CLI OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." "${CMAKE_CURRENT_BINARY_DIR}/bodyclock-core")

#-------------------------------------------------------------------------------
# The addon
#-------------------------------------------------------------------------------

add_library(bodyclock_addon SHARED src/binding.cpp ${CMAKE_JS_SRC})

# Node loads a file named <something>.node with no lib prefix, whatever the
# platform's own convention for a shared library is.
set_target_properties(bodyclock_addon PROPERTIES
    PREFIX ""
    SUFFIX ".node"
    OUTPUT_NAME "bodyclock"
)

target_include_directories(bodyclock_addon PRIVATE ${CMAKE_JS_INC})
target_link_libraries(bodyclock_addon PRIVATE bodyclock_core ${CMAKE_JS_LIB})

if(MSVC)
    target_compile_options(bodyclock_addon PRIVATE /utf-8 /W3)

    # cmake-js links the addon with /DELAYLOAD:NODE.EXE and compiles its own
    # delay-load hook in, which is how the Node-API imports get bound to
    # whichever host process loaded the module -- electron.exe here, not the
    # node.exe the import library names. The linker needs delayimp.lib for that
    # to resolve: without it the only error is one unresolved
    # __delayLoadHelper2, which says nothing about delay loading to anyone who
    # has not met it before.
    target_link_libraries(bodyclock_addon PRIVATE delayimp)

    # On Windows an addon imports the Node-API symbols from the host executable
    # rather than from a DLL, so the link needs an import library generated from
    # the runtime's .def. cmake-js hands over both paths; older cmake-js versions
    # and non-Windows generators leave them unset, hence the guard.
    if(CMAKE_JS_NODELIB_DEF AND CMAKE_JS_NODELIB_TARGET)
        execute_process(COMMAND ${CMAKE_AR} /def:${CMAKE_JS_NODELIB_DEF}
                                            /out:${CMAKE_JS_NODELIB_TARGET}
                                            ${CMAKE_STATIC_LINKER_FLAGS})
    endif()
else()
    target_compile_options(bodyclock_addon PRIVATE -Wall -Wextra)
endif()

if(APPLE)
    # The Node-API symbols are resolved by the host process at load time, not by
    # anything on the link line, so the default "undefined symbols are an error"
    # would fail this link.
    set_target_properties(bodyclock_addon PROPERTIES
        LINK_FLAGS "-undefined dynamic_lookup")
endif()
