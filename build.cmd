@echo off
rem The core library, the desktop tools in cli\ and the tests in tests\, into
rem build\. Not the desktop app -- that is desktop\build.cmd.
rem
rem The point of a desktop build is that the core can be exercised without a
rem device -- see cli\README.md. It is not a shipping target.
rem
rem With no arguments it configures if needed and builds, like every other
rem build script in this repo. help, -h or --help prints :usage below.
rem
rem `tts` assembles a qwentts.cpp SDK into build\qtts-sdk\ and configures with
rem -DBODYCLOCK_QTTS=ON -- speech output for the tools in cli\, without the
rem desktop app. See :qtts_sdk.
rem
rem `asr` assembles a desktop ggml SDK into build\llama-sdk\ from a llama.cpp
rem checkout and configures with -DBODYCLOCK_ASR=ON. Only ggml is built --
rem recognition does not link llama -- so this is a couple of minutes, not the
rem twenty the full engine takes.
setlocal EnableExtensions EnableDelayedExpansion

rem %~dp0 is NOT used below this point: `shift` moves %0 as well, so after
rem the argument loop it names an argument rather than this script.
set "PROJECT_DIR=%~dp0"
set "BUILD_DIR=%PROJECT_DIR%build"

set "DO_CLEAN="
set "DO_ASR="
set "DO_TTS="
:args
if "%~1"=="" goto args_done
if /I "%~1"=="clean" ( set "DO_CLEAN=1" & shift & goto args )
if /I "%~1"=="asr"   ( set "DO_ASR=1"   & shift & goto args )
if /I "%~1"=="tts"   ( set "DO_TTS=1"   & shift & goto args )
if /I "%~1"=="help"   goto usage
if /I "%~1"=="-h"     goto usage
if /I "%~1"=="--help" goto usage
echo Unknown argument "%~1". Run build.cmd --help for the options.>&2
exit /b 1
:args_done

rem YOAI_* settings, from the environment and from .env. See .env.example.
rem After the arguments, so the help text still prints when a configured
rem path is wrong. The names are the paths this script reads: each one that
rem is configured has to exist.
call "%PROJECT_DIR%scripts\load-env.cmd" VS_DIR NINJA VCPKG_ROOT LLAMA_SRC QTTS_SRC VULKAN_SDK || exit /b 1
set "THIRD_SYNC=%PROJECT_DIR%third\sync.cmd"

if defined DO_CLEAN if exist "%BUILD_DIR%" rd /s /q "%BUILD_DIR%"

if not defined VS_DIR set "VS_DIR=%ProgramFiles%\Microsoft Visual Studio\18\Community"
if not defined NINJA set "NINJA=%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

rem vcvarsall.bat overwrites VCPKG_ROOT with the VS-bundled copy; stash and
rem restore the user's value when they set one.
set "_USER_VCPKG_ROOT=%VCPKG_ROOT%"
call "%VS_DIR%\VC\Auxiliary\Build\vcvarsall.bat" amd64 >nul
if errorlevel 1 (
    echo Could not run vcvarsall.bat under "%VS_DIR%". Set VS_DIR.>&2
    exit /b 1
)
if defined _USER_VCPKG_ROOT set "VCPKG_ROOT=%_USER_VCPKG_ROOT%"

rem CMake caches which lanes are on, so adding `asr` or `tts` to a tree that
rem was configured without it would change nothing. Said here, before any SDK
rem is built, rather than after minutes of building one.
if exist "%BUILD_DIR%\build.ninja" (
    if defined DO_ASR (
        findstr /C:"BODYCLOCK_ASR:BOOL=ON" "%BUILD_DIR%\CMakeCache.txt" >nul 2>&1 || (
            echo [build] build\ was configured without asr; re-run as: build.cmd clean asr>&2
            exit /b 1
        )
    )
    if defined DO_TTS (
        findstr /C:"BODYCLOCK_QTTS:BOOL=ON" "%BUILD_DIR%\CMakeCache.txt" >nul 2>&1 || (
            echo [build] build\ was configured without tts; re-run as: build.cmd clean tts>&2
            exit /b 1
        )
    )
)

rem Two variables rather than one: the quotes have to wrap the whole -D argument
rem so a path with a space survives, and `set "NAME=..."` would put them inside
rem the value instead, leaving CMake with a directory whose name has quotes in it.
set "ASR_ON="
set "ASR_DIR="
if defined DO_ASR (
    call :ggml_sdk || exit /b 1
    set "ASR_ON=-DBODYCLOCK_ASR=ON"
    set ASR_DIR="-DLLAMA_CPP_DIR=%BUILD_DIR%\llama-sdk"
)
set "TTS_ON="
set "TTS_DIR="
if defined DO_TTS (
    call :qtts_sdk || exit /b 1
    set "TTS_ON=-DBODYCLOCK_QTTS=ON"
    set TTS_DIR="-DQTTS_DIR=%BUILD_DIR%\qtts-sdk"
)

if not exist "%BUILD_DIR%\build.ninja" (
    cmake -S "%PROJECT_DIR%." -B "%BUILD_DIR%" ^
        -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
        -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
        -DVCPKG_TARGET_TRIPLET=x64-windows ^
        -DVCPKG_INSTALLED_DIR="%PROJECT_DIR%vcpkg_installed" ^
        !ASR_ON! !ASR_DIR! !TTS_ON! !TTS_DIR!
    if errorlevel 1 exit /b 1
)

cmake --build "%BUILD_DIR%" --config Release
rem Propagate cmake's exit code. Bare `exit /b` resets ERRORLEVEL to 0 despite
rem the folklore, so a failed build would report success.
exit /b %errorlevel%

rem -----------------------------------------------------------------------------
rem Help. Exit 0: asking for it is not an error. No `!` in any line below --
rem delayed expansion is on and would eat it.
rem -----------------------------------------------------------------------------
:usage
echo Usage: build.cmd [clean] [asr] [tts]
echo.
echo Builds the core library, the desktop tools in cli\ and the tests in tests\,
echo into build\: configures if needed, then builds. This is not the desktop
echo app -- that is desktop\build.cmd.
echo.
echo   asr      also compile speech recognition in. Needs a llama.cpp checkout
echo            for ggml; see LLAMA_SRC
echo   tts      also compile speech output ^(Qwen3-TTS^) in. Needs qwentts.cpp
echo            and llama.cpp checkouts; runs on the GPU when VULKAN_SDK is set
echo   clean    delete build\ first
echo   help     this text; also -h or --help
echo.
echo They combine, e.g. build.cmd clean asr tts
echo.
echo Environment, each also as YOAI_^<NAME^> in the environment or in .env,
echo which wins -- see .env.example:
echo   VS_DIR       Visual Studio install root
echo   VCPKG_ROOT   vcpkg checkout (default: the VS-bundled one)
echo   NINJA        ninja.exe path
echo   LLAMA_SRC    llama.cpp checkout for asr and tts (default: ..\llama.cpp)
echo   QTTS_SRC     qwentts.cpp checkout for tts (default: ..\qwentts.cpp)
echo   VULKAN_SDK   Vulkan SDK, for tts on the GPU; optional
exit /b 0

rem -----------------------------------------------------------------------------
rem A desktop ggml SDK, shaped like the one harmony\build.cmd assembles per ABI:
rem include\ plus lib\. Recognition links ggml and nothing else, so only the three
rem ggml targets are built -- libllama is not needed and is not asked for.
rem -----------------------------------------------------------------------------
:ggml_sdk
if not defined LLAMA_SRC for %%I in ("%PROJECT_DIR%..") do set "LLAMA_SRC=%%~fI\llama.cpp"
if not exist "!LLAMA_SRC!\ggml\include\ggml.h" (
    echo no llama.cpp at "!LLAMA_SRC!".>&2
    echo clone it at the pinned commit, with its patch applied:>&2
    echo   third\sync.cmd clone llama.cpp>&2
    echo or point LLAMA_SRC at an existing checkout.>&2
    exit /b 1
)
call :third_verify llama.cpp LLAMA_SRC "!LLAMA_SRC!"

set "GBUILD=%BUILD_DIR%\ggml"
set "LSDK=%BUILD_DIR%\llama-sdk"
echo [build] ggml from !LLAMA_SRC!

if not exist "!GBUILD!\build.ninja" (
    cmake -S "!LLAMA_SRC!" -B "!GBUILD!" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
        -DBUILD_SHARED_LIBS=OFF ^
        -DLLAMA_BUILD_TESTS=OFF ^
        -DLLAMA_BUILD_EXAMPLES=OFF ^
        -DLLAMA_BUILD_TOOLS=OFF ^
        -DLLAMA_BUILD_SERVER=OFF ^
        -DLLAMA_CURL=OFF
    if errorlevel 1 exit /b 1
)
cmake --build "!GBUILD!" --target ggml ggml-cpu ggml-base
if errorlevel 1 exit /b 1

if not exist "!LSDK!\include" mkdir "!LSDK!\include"
if not exist "!LSDK!\lib" mkdir "!LSDK!\lib"
copy /y "!LLAMA_SRC!\ggml\include\*.h" "!LSDK!\include\" >nul

rem %GBUILD% and not !GBUILD!: `for /R` takes its root at parse time and does not
rem honour delayed expansion, so the exclamation form searches a directory
rem literally named "!GBUILD!", finds nothing, and copies nothing. The per-file
rem copy is allowed to fail -- `for /R` yields a candidate path in every
rem directory whether or not the file is there -- so the check afterwards is what
rem turns a silent miss into an error.
for /r "%GBUILD%" %%F in (ggml.lib ggml-cpu.lib ggml-base.lib) do copy /y "%%F" "!LSDK!\lib\" >nul 2>&1
if not exist "!LSDK!\lib\ggml.lib" (
    echo [build] ggml built but nothing was copied into !LSDK!\lib>&2
    exit /b 1
)
echo [build] ggml SDK at !LSDK!
exit /b 0

rem -----------------------------------------------------------------------------
rem A qwentts.cpp SDK for this build: include\ plus lib\, like the ggml SDK
rem above, so the core can speak without the desktop app. Its own copy rather
rem than desktop\sdk\qtts-sdk, which is built /MT for cmake-js: this build is /MD
rem like the vcpkg libraries it links, and the two do not mix in one link.
rem
rem qwentts.cpp and llama.cpp are both required. qwentts compiles llama.cpp's
rem ggml through GGML_SOURCE_DIR, so the core carries one ggml however many
rem lanes are on -- see "Exactly one ggml" in the root CMakeLists.txt.
rem
rem Vulkan is not required. Without VULKAN_SDK this builds for the CPU and says
rem so: the desktop measured the lane at RTF 1.13 on a laptop CPU against 0.63
rem on its integrated GPU, so it works, only slower than real time.
rem -----------------------------------------------------------------------------
:qtts_sdk
if not defined QTTS_SRC for %%I in ("%PROJECT_DIR%..") do set "QTTS_SRC=%%~fI\qwentts.cpp"
if not exist "!QTTS_SRC!\src\qwen.h" (
    echo no qwentts.cpp at "!QTTS_SRC!".>&2
    echo clone it at the pinned commit, submodule and patch included:>&2
    echo   third\sync.cmd clone qwentts.cpp>&2
    echo or point QTTS_SRC at an existing checkout.>&2
    exit /b 1
)
call :third_verify qwentts.cpp QTTS_SRC "!QTTS_SRC!"
if not defined LLAMA_SRC for %%I in ("%PROJECT_DIR%..") do set "LLAMA_SRC=%%~fI\llama.cpp"
if not exist "!LLAMA_SRC!\ggml\CMakeLists.txt" (
    echo tts compiles llama.cpp's ggml, and there is none at "!LLAMA_SRC!\ggml".>&2
    echo   third\sync.cmd clone llama.cpp>&2
    echo or point LLAMA_SRC at an existing checkout.>&2
    exit /b 1
)
call :third_verify llama.cpp LLAMA_SRC "!LLAMA_SRC!"

rem SPIRV-Headers is named explicitly, for the reason desktop\build.cmd gives at
rem its :qtts_sdk: ggml's own search does not find the LunarG SDK's layout.
set "QVULKAN=OFF"
set "QSPIRV="
if not defined VULKAN_SDK goto qtts_no_vulkan
set "QVULKAN=ON"
set "QVK=!VULKAN_SDK:\=/!"
if exist "!VULKAN_SDK!\Lib\cmake\SPIRV-Headers\SPIRV-HeadersConfig.cmake" set QSPIRV="-DSPIRV-Headers_DIR=!QVK!/Lib/cmake/SPIRV-Headers"
if not defined QSPIRV if exist "!VULKAN_SDK!\Lib\cmake\SPIRV-HeadersConfig.cmake" set QSPIRV="-DSPIRV-Headers_DIR=!QVK!/Lib/cmake"
echo [build] tts backend: Vulkan, from !VULKAN_SDK!
goto qtts_configure
:qtts_no_vulkan
echo [build] warning: VULKAN_SDK is not set, so tts is built for the CPU only.>&2
echo [build]   It works, slower: RTF 1.13 against 0.63 on the GPU where measured.>&2
echo [build]   For the GPU, install the Vulkan SDK or set YOAI_VULKAN_SDK in .env,>&2
echo [build]   then build.cmd clean tts -- the core only looks for it when configured.>&2
:qtts_configure

set "QBUILD=%BUILD_DIR%\qtts"
set "QSDK=%BUILD_DIR%\qtts-sdk"
echo [build] qwentts.cpp from !QTTS_SRC!, ggml from !LLAMA_SRC!\ggml

rem Configured every run, with GGML_VULKAN passed either way, so the backend
rem follows VULKAN_SDK rather than whatever the first run cached.
cmake -S "!QTTS_SRC!" -B "!QBUILD!" -G Ninja ^
    "-DCMAKE_MAKE_PROGRAM=%NINJA%" ^
    -DCMAKE_BUILD_TYPE=Release ^
    "-DGGML_SOURCE_DIR=!LLAMA_SRC:\=/!/ggml" ^
    -DBUILD_SHARED_LIBS=OFF ^
    -DQWEN_SHARED=OFF ^
    !QSPIRV! ^
    -DGGML_VULKAN=!QVULKAN!
if errorlevel 1 exit /b 1
rem qwen-core only: qwen-tts, qwen-codec and tts-server all want a main().
cmake --build "!QBUILD!" --config Release --target qwen-core
if errorlevel 1 exit /b 1

rem Laid out from empty every run. The libraries are collected by search, because
rem ggml splits its archives differently between versions, and a tree that was
rem once built with Vulkan keeps its ggml-vulkan.lib after Vulkan is turned off:
rem the core links whatever this directory holds, so that one is taken out.
if exist "!QSDK!" rmdir /s /q "!QSDK!"
mkdir "!QSDK!\include"
mkdir "!QSDK!\lib"
copy /y "!QTTS_SRC!\src\qwen.h" "!QSDK!\include\" >nul
rem %BUILD_DIR% and not !QBUILD!: `for /R` takes its root at parse time.
for /r "%BUILD_DIR%\qtts" %%F in (*.lib) do copy /y "%%~fF" "!QSDK!\lib\" >nul
if "!QVULKAN!"=="OFF" if exist "!QSDK!\lib\ggml-vulkan.lib" del "!QSDK!\lib\ggml-vulkan.lib"
if not exist "!QSDK!\lib\qwen-core.lib" (
    echo [build] qwentts built, but qwen-core.lib is not in !QSDK!\lib>&2
    exit /b 1
)
if "!QVULKAN!"=="ON" if not exist "!QSDK!\lib\ggml-vulkan.lib" (
    echo [build] qwentts built WITHOUT the Vulkan backend -- check that cmake found it>&2
    exit /b 1
)
echo [build] tts SDK at !QSDK!
exit /b 0

rem -----------------------------------------------------------------------------
rem Is this checkout what third\manifest.txt records? %1 the manifest name, %2
rem its variable there, %3 the tree this build is about to compile.
rem
rem A WARNING, never a stop: a tree ahead of its pin is a normal state while
rem trying something (third\README.md), and a missing patch matters to some
rem lanes and not others. The tree goes over as YOAI_<var>, the one name sync's
rem own loader will not override, so the tree checked is the tree built. Once
rem per name per run.
rem -----------------------------------------------------------------------------
:third_verify
if defined THIRD_SEEN_%~1 exit /b 0
setlocal
set "YOAI_%~2=%~3"
call "%THIRD_SYNC%" verify %~1
if errorlevel 1 echo [build] warning: %~1 differs from third\manifest.txt ^(above^) -- building anyway. Details: third\sync.cmd check %~1>&2
endlocal & set "THIRD_SEEN_%~1=1"
exit /b 0
