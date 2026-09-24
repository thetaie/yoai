@echo off
rem The core library, the desktop tools in cli\ and the tests in tests\, into
rem build\. Not the desktop app -- that is desktop\build.cmd.
rem
rem The point of a desktop build is that the core can be exercised without a
rem device -- see cli\README.md. It is not a shipping target.
rem
rem With no arguments this prints :usage below and builds nothing, so running
rem the script to find out what it does is safe.
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
if "%~1"=="" goto usage
:args
if "%~1"=="" goto args_done
rem `build` sets nothing: every command builds, and this one only asks for the
rem defaults.
if /I "%~1"=="build" ( shift & goto args )
if /I "%~1"=="clean" ( set "DO_CLEAN=1" & shift & goto args )
if /I "%~1"=="asr"   ( set "DO_ASR=1"   & shift & goto args )
if /I "%~1"=="help"   goto usage
if /I "%~1"=="-h"     goto usage
if /I "%~1"=="--help" goto usage
echo Unknown argument "%~1". Run build.cmd with no arguments for help.>&2
exit /b 1
:args_done

rem YOAI_* settings, from the environment and from .env. See .env.example.
rem After the arguments, so the help text still prints when a configured
rem path is wrong. The names are the paths this script reads: each one that
rem is configured has to exist.
call "%PROJECT_DIR%scripts\load-env.cmd" VS_DIR NINJA VCPKG_ROOT LLAMA_SRC || exit /b 1
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

rem CMake caches everything below, so adding `asr` to a build tree that was
rem configured without it has no effect. Re-run with `clean` to change the set.
if not exist "%BUILD_DIR%\build.ninja" (
    cmake -S "%PROJECT_DIR%." -B "%BUILD_DIR%" ^
        -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
        -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
        -DVCPKG_TARGET_TRIPLET=x64-windows ^
        -DVCPKG_INSTALLED_DIR="%PROJECT_DIR%vcpkg_installed" ^
        !ASR_ON! !ASR_DIR!
    if errorlevel 1 exit /b 1
) else (
    if defined DO_ASR (
        findstr /C:"BODYCLOCK_ASR:BOOL=ON" "%BUILD_DIR%\CMakeCache.txt" >nul 2>&1 || (
            echo [build] build\ was configured without asr; re-run as: build.cmd clean asr>&2
            exit /b 1
        )
    )
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
echo Usage: build.cmd ^<command^> [command ...]
echo.
echo Builds the core library, the desktop tools in cli\ and the tests in tests\,
echo into build\. This is not the desktop app -- that is desktop\build.cmd.
echo.
echo Commands:
echo   build    configure if needed, then build
echo   asr      the same, with speech recognition compiled in. Needs a llama.cpp
echo            checkout for ggml; see LLAMA_SRC
echo   clean    delete build\ first, then build
echo.
echo Commands combine, e.g. build.cmd clean asr
echo.
echo Environment, each also as YOAI_^<NAME^> in the environment or in .env,
echo which wins -- see .env.example:
echo   VS_DIR       Visual Studio install root
echo   VCPKG_ROOT   vcpkg checkout (default: the VS-bundled one)
echo   NINJA        ninja.exe path
echo   LLAMA_SRC    llama.cpp checkout for asr (default: ..\llama.cpp)
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
