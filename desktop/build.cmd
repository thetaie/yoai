@echo off
rem Desktop client build: the Node-API addon, then the Electron bundles.
rem
rem The addon is the slow half and changes rarely; the bundles are seconds. They
rem are separate arguments for that reason -- the loop while writing UI is
rem `npm run dev`, and nothing here has to run again until the C++ changes.
rem
rem Usage: build.cmd [clean] [addon] [llama] [qtts] [mnn] [app]
rem
rem   (no args)  addon + app
rem   addon      just the Node-API addon (build\Release\bodyclock.node)
rem   llama      assemble a desktop llama.cpp SDK and build the addon WITH the
rem              on-device lane. Implies addon. Recognition rides along: it
rem              links ggml and never libllama, so the same SDK turns it on.
rem   qtts       assemble a desktop qwentts.cpp SDK WITH the Vulkan backend and
rem              build the addon with speech output. Implies addon.
rem              Vulkan is not a nicety here: this lane measures RTF 1.13 on
rem              this CPU and 0.63 on its integrated GPU, and only below 1 can
rem              a stream start without pre-buffering the whole utterance.
rem              Needs a Vulkan SDK on PATH (VULKAN_SDK) for glslc and the C++
rem              headers ggml-vulkan includes.
rem   mnn        assemble a desktop MNN SDK and build the addon with the omni
rem              lane: audio in, a spoken answer out, no text in between.
rem              Implies addon. A second inference engine and 3.8 GB of
rem              weights -- see design/voice.md before turning it on.
rem   app        just the Electron bundles (out\)
rem   clean      discard build\ first. Needed to change the lane set: CMake
rem              caches it, so adding `llama` to a tree configured without it
rem              has no effect.
rem
rem Env overrides:
rem   VS_DIR       Visual Studio install root
rem   NINJA        ninja.exe path
rem   VCPKG_ROOT   vcpkg checkout (default: the VS-bundled one)
rem   LLAMA_SRC    llama.cpp checkout (default: a sibling of the repo), `llama` only
rem   QTTS_SRC     qwentts.cpp checkout (default: a sibling), `qtts` only
rem   MNN_SRC      MNN checkout (default: a sibling), `mnn` only
setlocal EnableExtensions EnableDelayedExpansion

set "DESKTOP_DIR=%~dp0"
if "%DESKTOP_DIR:~-1%"=="\" set "DESKTOP_DIR=%DESKTOP_DIR:~0,-1%"
for %%I in ("%DESKTOP_DIR%\..") do set "PROJECT_DIR=%%~fI"
set "BUILD_DIR=%DESKTOP_DIR%\build"

rem The SDKs live OUTSIDE build\, and that is not tidiness. build\ belongs to
rem cmake-js: when its build step fails it runs its own CLEAN, which deletes the
rem whole output directory. With the SDKs inside, one compile error in
rem binding.cpp costs a full llama.cpp rebuild and a full 151-shader Vulkan
rem rebuild -- minutes, for a typo. `clean` below still only discards build\,
rem so the SDKs survive a lane change too, which is the other thing that used to
rem rebuild them for nothing.
set "SDK_DIR=%DESKTOP_DIR%\sdk"

set "DO_CLEAN="
set "DO_ADDON="
set "DO_LLAMA="
set "DO_QTTS="
set "DO_MNN="
set "DO_APP="
:args
if "%~1"=="" goto args_done
if /I "%~1"=="clean" ( set "DO_CLEAN=1" & shift & goto args )
if /I "%~1"=="addon" ( set "DO_ADDON=1" & shift & goto args )
if /I "%~1"=="llama" ( set "DO_LLAMA=1" & set "DO_ADDON=1" & shift & goto args )
if /I "%~1"=="qtts"  ( set "DO_QTTS=1"  & set "DO_ADDON=1" & shift & goto args )
if /I "%~1"=="mnn"   ( set "DO_MNN=1"   & set "DO_ADDON=1" & shift & goto args )
if /I "%~1"=="app"   ( set "DO_APP=1"   & shift & goto args )
echo Unknown argument "%~1". Usage: build.cmd [clean] [addon] [llama] [qtts] [mnn] [app]>&2
exit /b 1
:args_done

rem No target named means both, which is what a first run wants.
if not defined DO_ADDON if not defined DO_APP (
    set "DO_ADDON=1"
    set "DO_APP=1"
)

if defined DO_CLEAN if exist "%BUILD_DIR%" rd /s /q "%BUILD_DIR%"

if not exist "%DESKTOP_DIR%\node_modules" (
    echo [build] npm install
    call npm --prefix "%DESKTOP_DIR%" install
    if errorlevel 1 exit /b 1
)

if defined DO_ADDON call :addon || exit /b 1
if defined DO_APP   call :app   || exit /b 1
exit /b 0

rem -----------------------------------------------------------------------------
rem The Node-API addon
rem -----------------------------------------------------------------------------
:addon
if not defined VS_DIR set "VS_DIR=%ProgramFiles%\Microsoft Visual Studio\18\Community"
if not defined NINJA set "NINJA=%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

rem vcvarsall.bat overwrites VCPKG_ROOT with the VS-bundled copy; stash and
rem restore the user's value when they set one. Same dance as the root build.
set "_USER_VCPKG_ROOT=%VCPKG_ROOT%"
call "%VS_DIR%\VC\Auxiliary\Build\vcvarsall.bat" amd64 >nul
if errorlevel 1 (
    echo Could not run vcvarsall.bat under "%VS_DIR%". Set VS_DIR.>&2
    exit /b 1
)
if defined _USER_VCPKG_ROOT set "VCPKG_ROOT=%_USER_VCPKG_ROOT%"

rem The addon is loaded by Electron, not by Node, so it is built against
rem Electron's headers. Read from the installed package rather than pinned here:
rem a version that disagrees with the one in node_modules is a module that
rem refuses to load, and the error names an ABI rather than a version.
for /f "delims=" %%V in ('node -p "require('electron/package.json').version" 2^>nul') do set "ELECTRON_VER=%%V"
if not defined ELECTRON_VER (
    echo Could not read electron's version. Run `npm install` in desktop\ first.>&2
    exit /b 1
)
echo [build] addon against electron !ELECTRON_VER!

set "LLAMA_ON="
set "LLAMA_DIR="
if defined DO_LLAMA (
    call :llama_sdk || exit /b 1
    set "LLAMA_ON=--CDBODYCLOCK_ONDEVICE_LLM=ON"
    set LLAMA_DIR="--CDLLAMA_CPP_DIR=%SDK_DIR%\llama-sdk"
)

set "QTTS_ON="
set "QTTS_DIR_ARG="
if defined DO_QTTS (
    call :qtts_sdk || exit /b 1
    set "QTTS_ON=--CDBODYCLOCK_QTTS=ON"
    set QTTS_DIR_ARG="--CDQTTS_DIR=%SDK_DIR%\qtts-sdk"
)

set "MNN_ON="
set "MNN_DIR_ARG="
set "MNN_VK_ON="
if defined DO_MNN (
    call :mnn_sdk || exit /b 1
    set "MNN_ON=--CDBODYCLOCK_MNN=ON"
    set MNN_DIR_ARG="--CDMNN_DIR=%SDK_DIR%\mnn-sdk"
    rem Off, with -DMNN_VULKAN under :mnn_sdk and for the same reason. The two
    rem travel together: this one says the Vulkan symbols exist to link against,
    rem and referencing them in a libMNN built without the backend does not link
    rem at all.
    rem set "MNN_VK_ON=--CDBODYCLOCK_MNN_VULKAN=ON"
)

rem Two things here are worked around rather than configured, and both cost an
rem afternoon to find a second time:
rem
rem  - `node <entry>` and NOT `npx cmake-js`. npx prepends node_modules\.bin to
rem    PATH for everything it spawns, and one of cmake-js's own dependencies
rem    installs a shim there called `rc`. CMake then picks that up as the
rem    RESOURCE COMPILER instead of the Windows SDK's rc.exe, and the failure is
rem    "RC Pass 1 ... unknown error" while probing whether cl.exe works at all.
rem
rem  - `-G Ninja` and not cmake-js's own choice. Its Visual Studio detection
rem    finds VS 2026 and then reports "There is no Visual C++ compiler
rem    installed", because it does not know that generator's name. vcvarsall has
rem    already put cl.exe on PATH by this point, so Ninja needs nothing from it.
rem    CMAKE_BUILD_TYPE has to be named with a single-config generator, and it
rem    must be Release: vcpkg's x64-windows libraries are /MD, and a Debug addon
rem    would be /MDd against them.
rem
rem Quoting: the whole --CD argument goes inside the quotes so a path with a
rem space survives as one token. `set "NAME=..."` would put the quotes in the
rem value instead, handing CMake a directory whose name contains them.
node "%DESKTOP_DIR%\node_modules\cmake-js\bin\cmake-js" compile ^
    --directory "%DESKTOP_DIR%\native" ^
    --out "%BUILD_DIR%" ^
    --runtime electron ^
    --runtime-version !ELECTRON_VER! ^
    --generator Ninja ^
    "--CDCMAKE_MAKE_PROGRAM=%NINJA%" ^
    --CDCMAKE_BUILD_TYPE=Release ^
    "--CDCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
    --CDVCPKG_TARGET_TRIPLET=x64-windows ^
    "--CDVCPKG_INSTALLED_DIR=%PROJECT_DIR%\vcpkg_installed" ^
    !LLAMA_ON! !LLAMA_DIR! ^
    !QTTS_ON! !QTTS_DIR_ARG! ^
    !MNN_ON! !MNN_DIR_ARG! !MNN_VK_ON!
if errorlevel 1 exit /b 1

rem Ninja writes it at the root of the output directory; a multi-config
rem generator would put it under Release\. Both are checked because the module
rem loader in src\main\core.ts searches both, and this message is the one place
rem that can say which one actually happened.
set "ADDON=%BUILD_DIR%\bodyclock.node"
if not exist "!ADDON!" set "ADDON=%BUILD_DIR%\Release\bodyclock.node"
if not exist "!ADDON!" (
    echo [build] cmake-js reported success but there is no bodyclock.node under %BUILD_DIR%>&2
    exit /b 1
)
echo [build] addon at !ADDON!
exit /b 0

rem -----------------------------------------------------------------------------
rem The Electron bundles
rem -----------------------------------------------------------------------------
:app
echo [build] electron-vite
call npm --prefix "%DESKTOP_DIR%" run build
exit /b %errorlevel%

rem -----------------------------------------------------------------------------
rem A desktop qwentts.cpp SDK, shaped like the llama one: include\ plus lib\.
rem
rem WITH THE VULKAN BACKEND, which is the whole point of building it here. On
rem this laptop's CPU the lane measures 1.13x real time and on its integrated
rem Arc 0.63x, and below 1 is a threshold rather than a slope: a stream needs a
rem pre-buffer of duration x (RTF - 1), which at 0.63 is nothing and at 1.13 is
rem eight seconds for a minute of audio.
rem
rem Far fewer flags than harmony\build.cmd's cross build needs. Three of the
rem four obstacles there were artefacts of cross-compiling -- SPIRV-Headers
rem re-rooted into the sysroot, a nested host build inheriting the generator
rem but not CMAKE_MAKE_PROGRAM, and a sysroot carrying Vulkan's C headers but
rem not the C++ ones. On the host, find_package(Vulkan) and ggml's own
rem CMAKE_PREFIX_PATH append of VULKAN_SDK both just work.
rem
rem One thing does carry over: ggml builds vulkan-shaders-gen for the host and
rem runs it over 151 shaders, and MSVC fails that nested configure with
rem "Cannot open compiler generated file" when the path gets long. build\qtts
rem here is short enough; a build directory nested much deeper is not.
rem -----------------------------------------------------------------------------
:qtts_sdk
if not defined QTTS_SRC for %%I in ("%PROJECT_DIR%\..") do set "QTTS_SRC=%%~fI\qwentts.cpp"
if not exist "!QTTS_SRC!\src\qwen.h" (
    echo no qwentts.cpp at "!QTTS_SRC!".>&2
    echo clone it at the pinned commit, submodule and patches included:>&2
    echo   ..\third\sync.cmd clone qwentts.cpp>&2
    echo or point QTTS_SRC at an existing checkout.>&2
    exit /b 1
)
if not defined VULKAN_SDK (
    echo qtts needs a Vulkan SDK ^(VULKAN_SDK is unset^) for glslc and vulkan.hpp.>&2
    exit /b 1
)

rem ONE ggml for both lanes, and llama.cpp's is the one. qwentts.cpp builds a
rem ggml of its own from a fork, and until 2026-09-21 that copy superset
rem llama.cpp's, so linking qwentts's satisfied both libraries. The update
rem inverted it -- libllama now wants ggml_dsv4_hc_pre_gated, which the fork
rem does not have -- and whichever copy is kept, one symbol goes unmet.
rem
rem Linking llama's copy instead is not an option HERE specifically: only the
rem qtts configure asks for GGML_VULKAN, so llama's SDK has no ggml-vulkan.lib,
rem and Vulkan is the entire reason this lane exists (RTF 0.63 on the iGPU
rem against 1.13 on the CPU). GGML_SOURCE_DIR, which qwentts added the same day,
rem is the way out: qwentts compiles llama.cpp's ggml, with Vulkan, and the one
rem archive set satisfies qwen-core's 134 symbols and libllama's 288 both.
rem
rem What it costs is real and unmeasured: the fork carries
rem ggml_compute_forward_snake_fused, a graph pattern match in ggml-cpu.c that
rem collapses the DAC decoder's snake activation into one kernel. llama's ggml
rem has no such fusion, so those five ops run unfused -- same output, some
rem unknown amount slower, in the codec stage that is already 45% of the bill.
if not defined GGML_SRC set "GGML_SRC=!LLAMA_SRC!"
if not defined GGML_SRC for %%I in ("%PROJECT_DIR%\..") do set "GGML_SRC=%%~fI\llama.cpp"
if not exist "!GGML_SRC!\ggml\CMakeLists.txt" (
    echo qtts needs llama.cpp's ggml at "!GGML_SRC!\ggml" to build against.>&2
    echo   ..\third\sync.cmd clone llama.cpp>&2
    exit /b 1
)

set "QBUILD=%SDK_DIR%\qtts"
set "QSDK=%SDK_DIR%\qtts-sdk"
echo [build] qwentts.cpp from !QTTS_SRC!, ggml from !GGML_SRC!\ggml, Vulkan from !VULKAN_SDK!

rem CMAKE_MSVC_RUNTIME_LIBRARY is not a preference, and it is not optional.
rem cmake-js forces /MT on the addon unconditionally (cMake.js: "MD builds will
rem crash upon attempting to free memory"), CMake's own default is /MD, and MSVC
rem records the choice in every .obj. Mixing them is LNK2038 once per translation
rem unit plus a page of unresolved __imp_* -- which is what a link against an
rem unflagged SDK looks like, and it says nothing about the CRT in the message.
rem
rem CMAKE_POLICY_DEFAULT_CMP0091=NEW is what makes the line above do anything
rem here. qwentts.cpp declares cmake_minimum_required(VERSION 3.14), so CMP0091
rem is OLD for its whole tree, and under OLD the runtime library is not an
rem abstraction at all -- /MD is a literal string inside CMAKE_CXX_FLAGS_RELEASE
rem and CMAKE_MSVC_RUNTIME_LIBRARY is read by nobody. llama.cpp asks for a newer
rem CMake, so it obeyed the flag and qwentts silently did not; the link error
rem then named only the three ggml archives, which look like the common half.
rem
rem Configured every run, not only when CMakeCache.txt is absent. The guard that
rem used to be here saved a few seconds and cost exactly this bug: a flag added
rem above has no effect on a tree already configured without it, so the build
rem keeps succeeding and the addon keeps failing to link. CMake is idempotent
rem and quick when nothing changed; the same applies to :llama_sdk below.
(
    cmake -S "!QTTS_SRC!" -B "!QBUILD!" -G Ninja ^
        "-DCMAKE_MAKE_PROGRAM=%NINJA%" ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_POLICY_DEFAULT_CMP0091=NEW ^
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
        "-DGGML_SOURCE_DIR=!GGML_SRC:\=/!/ggml" ^
        -DBUILD_SHARED_LIBS=OFF ^
        -DQWEN_SHARED=OFF ^
        -DGGML_VULKAN=ON
    if errorlevel 1 exit /b 1
)

rem Re-running cmake cannot take /MD back out of a cache that already has it:
rem CMAKE_CXX_FLAGS_RELEASE is initialised once and then kept. So the two flags
rem above only work on a tree configured WITH them, and a tree configured
rem without them keeps building happily and failing at the addon's link. Say so
rem here, where the cache is, rather than 300 lines of LNK2038 later.
findstr /C:"CMAKE_CXX_FLAGS_RELEASE:STRING" "!QBUILD!\CMakeCache.txt" | findstr /C:"/MD" >nul
if not errorlevel 1 (
    echo [build] the qtts tree at !QBUILD! is configured for the /MD runtime,>&2
    echo [build] which cannot link into cmake-js's /MT addon. Delete it and rebuild:>&2
    echo [build]   rmdir /s /q "!QBUILD!">&2
    exit /b 1
)

cmake --build "!QBUILD!" --config Release --target qwen-core
if errorlevel 1 exit /b 1

if not exist "!QSDK!\include" mkdir "!QSDK!\include"
if not exist "!QSDK!\lib" mkdir "!QSDK!\lib"
copy /y "!QTTS_SRC!\src\qwen.h" "!QSDK!\include\" >nul

rem %QBUILD% and not !QBUILD!, for the reason spelled out under :llama_sdk --
rem `for /R` takes its root at parse time. Collected by search rather than by
rem name because ggml splits its archives differently between versions, and the
rem Vulkan backend adds one more; a missing one surfaces as an undefined symbol
rem at the addon's link step instead of here.
set "QLIBS=0"
for /r "%SDK_DIR%\qtts" %%F in (*.lib) do (
    copy /y "%%~fF" "!QSDK!\lib\" >nul
    set /a QLIBS+=1
)
if not exist "!QSDK!\lib\qwen-core.lib" (
    echo [build] qwentts built but qwen-core.lib is not in !QSDK!\lib>&2
    exit /b 1
)
if not exist "!QSDK!\lib\ggml-vulkan.lib" (
    echo [build] qwentts built WITHOUT the Vulkan backend -- check that cmake found it>&2
    exit /b 1
)
echo [build] qtts SDK at !QSDK! ^(!QLIBS! libraries^)
exit /b 0

rem -----------------------------------------------------------------------------
rem A desktop llama.cpp SDK, shaped like the ones harmony\build.cmd assembles
rem per ABI: include\ plus lib\. Unlike the root build's `asr`, this one needs
rem libllama as well as ggml, so it is the long build -- minutes, not one.
rem -----------------------------------------------------------------------------
:llama_sdk
if not defined LLAMA_SRC for %%I in ("%PROJECT_DIR%\..") do set "LLAMA_SRC=%%~fI\llama.cpp"
if not exist "!LLAMA_SRC!\ggml\include\ggml.h" (
    echo no llama.cpp at "!LLAMA_SRC!".>&2
    echo clone it at the pinned commit, with its patch applied:>&2
    echo   ..\third\sync.cmd clone llama.cpp>&2
    echo or point LLAMA_SRC at an existing checkout.>&2
    exit /b 1
)

set "LBUILD=%SDK_DIR%\llama"
set "LSDK=%SDK_DIR%\llama-sdk"
echo [build] llama.cpp from !LLAMA_SRC!

rem Configured every run, and CMAKE_MSVC_RUNTIME_LIBRARY is mandatory: see the
rem note above :qtts_sdk for both.
(
    cmake -S "!LLAMA_SRC!" -B "!LBUILD!" ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
        -DBUILD_SHARED_LIBS=OFF ^
        -DLLAMA_BUILD_TESTS=OFF ^
        -DLLAMA_BUILD_EXAMPLES=OFF ^
        -DLLAMA_BUILD_TOOLS=OFF ^
        -DLLAMA_BUILD_SERVER=OFF ^
        -DLLAMA_CURL=OFF
    if errorlevel 1 exit /b 1
)
cmake --build "!LBUILD!" --config Release --target llama ggml ggml-cpu ggml-base
if errorlevel 1 exit /b 1

if not exist "!LSDK!\include" mkdir "!LSDK!\include"
if not exist "!LSDK!\lib" mkdir "!LSDK!\lib"
copy /y "!LLAMA_SRC!\ggml\include\*.h" "!LSDK!\include\" >nul
copy /y "!LLAMA_SRC!\include\llama.h"  "!LSDK!\include\" >nul

rem %LBUILD% and not !LBUILD!: `for /R` takes its root at parse time and does
rem not honour delayed expansion, so the exclamation form would search a
rem directory literally named "!LBUILD!". The per-file copy is allowed to fail
rem -- `for /R` yields a candidate path in every directory whether or not the
rem file is there -- so the check afterwards is what turns a silent miss into an
rem error.
for /r "%SDK_DIR%\llama" %%F in (llama.lib ggml.lib ggml-cpu.lib ggml-base.lib) do copy /y "%%F" "!LSDK!\lib\" >nul 2>&1
if not exist "!LSDK!\lib\llama.lib" (
    echo [build] llama built but nothing was copied into !LSDK!\lib>&2
    exit /b 1
)
echo [build] llama SDK at !LSDK!
exit /b 0

rem -----------------------------------------------------------------------------
rem A desktop MNN SDK, shaped like the other two: include\ plus lib\.
rem
rem The SECOND inference engine, and the only reason to carry one: MNN has the
rem omni Talker -- thinker to codec tokens to a diffusion transformer to a
rem vocoder -- and llama.cpp has no Talker checkpoint in GGUF at all. This is the
rem end-to-end lane: audio in, a spoken answer out, with no text in the middle.
rem
rem MNN_WIN_RUNTIME_MT is the CRT, and it is not optional. MNN sets CMP0091 NEW
rem itself and then picks MultiThreadedDLL unless this is on; cmake-js forces /MT
rem on the addon, and the mismatch is the same page of LNK2038 the qtts lane
rem taught. Unlike qwentts, MNN has its own switch for it, so no policy override
rem is needed here.
rem
rem MNN_SEP_BUILD=OFF goes with MNN_BUILD_SHARED_LIBS=OFF: MNN has its own
rem shared-library switch and ignores the standard one, and with the two off the
rem llm engine becomes an OBJECT library folded into libMNN rather than a second
rem archive. There is no llm library to find, and looking for one reports a good
rem build as a failure.
rem
rem MNN_VULKAN is DELIBERATELY ABSENT, and the reason is a measurement rather
rem than an omission. Turning it on needs three things together: -DMNN_VULKAN=ON,
rem -DMNN_VULKAN_IMAGE=OFF (the default image path is the classic CNN backend --
rem no attention, no layernorm, no low-bit weights, so the DiT cannot run on it),
rem and BODYCLOCK_MNN_VULKAN below, because a static libMNN drops every one of
rem the backend's registrars unless the archive is linked whole. All three are
rem in place; see CMakeLists.txt and third/patches.
rem
rem The idea was sound and the first half of it held. The model's config.json has
rem two runtimes -- the main one carries the thinker and the Talker, the `mllm`
rem block carries the audio encoder, the diffusion transformer and the vocoder --
rem so moving only `mllm` to the GPU should overlap them properly. Today they run
rem on separate threads and the same four cores, which is why the split adds up
rem to the wall clock instead of hiding behind it. With `mllm` on Vulkan the
rem Talker's decode did drop from 3.57 s to 1.91 s, exactly as predicted.
rem
rem What it produced was noise. Vulkan returned -4, VK_ERROR_DEVICE_LOST, several
rem times per chunk: the GPU work never completed, so token2wav "finished" in
rem 0.87 s against 3.07 s on the CPU and the buffers it left behind were garbage.
rem The whole exchange was 7.5 s against 6.5 s.
rem
rem So it is off until there is a machine, or an MNN, where that does not happen.
rem -----------------------------------------------------------------------------
:mnn_sdk
if not defined MNN_SRC for %%I in ("%PROJECT_DIR%\..") do set "MNN_SRC=%%~fI\MNN"
if not exist "!MNN_SRC!\CMakeLists.txt" (
    echo no MNN at "!MNN_SRC!".>&2
    echo clone it at the pinned commit:>&2
    echo   ..\third\sync.cmd clone MNN>&2
    echo or point MNN_SRC at an existing checkout.>&2
    exit /b 1
)

set "MBUILD=%SDK_DIR%\mnn"
set "MSDK=%SDK_DIR%\mnn-sdk"
echo [build] MNN from !MNN_SRC!

rem Configured every run, for the reason spelled out under :qtts_sdk.
(
    cmake -S "!MNN_SRC!" -B "!MBUILD!" -G Ninja ^
        "-DCMAKE_MAKE_PROGRAM=%NINJA%" ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DMNN_WIN_RUNTIME_MT=ON ^
        -DBUILD_SHARED_LIBS=OFF ^
        -DMNN_BUILD_SHARED_LIBS=OFF ^
        -DMNN_SEP_BUILD=OFF ^
        -DMNN_BUILD_LLM=ON ^
        -DMNN_BUILD_LLM_OMNI=ON ^
        -DMNN_LOW_MEMORY=ON ^
        -DMNN_SUPPORT_TRANSFORMER_FUSE=ON ^
        -DMNN_BUILD_TOOLS=OFF ^
        -DMNN_BUILD_BENCHMARK=OFF ^
        -DMNN_BUILD_TEST=OFF
    if errorlevel 1 exit /b 1
)
cmake --build "!MBUILD!" --config Release --target MNN llm
if errorlevel 1 exit /b 1

echo [build] assembling !MSDK!
if not exist "!MSDK!\include" mkdir "!MSDK!\include"
if not exist "!MSDK!\lib"     mkdir "!MSDK!\lib"
xcopy /e /i /y /q "!MNN_SRC!\include\MNN" "!MSDK!\include\MNN" >nul || exit /b 1
xcopy /e /i /y /q "!MNN_SRC!\transformers\llm\engine\include\llm" "!MSDK!\include\llm" >nul || exit /b 1

rem And MNN's OWN rapidjson, which is not a convenience -- it is the fix for a
rem crash. rapidjson is header-only templates, so its member functions land in
rem the object files of whoever includes it, as weak symbols; the whole addon is
rem one module and the linker keeps exactly ONE definition of each. This repo
rem already carries a much newer rapidjson for src/llm and src/platform, and
rem libMNN was compiled against the 1.1.0 release it vendors. Two layouts, one
rem symbol: measured as a SIGSEGV at 0x10 inside MemoryPoolAllocator::Malloc, on
rem the FIRST json parse of Llm::createLLM, before a line of MNN's own logging.
xcopy /e /i /y /q "!MNN_SRC!\3rd_party\rapidjson" "!MSDK!\include\rapidjson" >nul || exit /b 1
if not exist "!MSDK!\include\rapidjson\document.h" (
    echo [build] no rapidjson under !MSDK!\include>&2
    echo [build] MNN vendors it at 3rd_party\rapidjson -- look there>&2
    exit /b 1
)

set "MLIBS=0"
for /r "%SDK_DIR%\mnn" %%F in (MNN.lib) do (
    copy /y "%%~fF" "!MSDK!\lib\" >nul
    set /a MLIBS+=1
)
if not exist "!MSDK!\lib\MNN.lib" (
    echo [build] MNN built but MNN.lib is not in !MSDK!\lib>&2
    exit /b 1
)
echo [build] MNN SDK at !MSDK!
exit /b 0
