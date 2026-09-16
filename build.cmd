@echo off
rem Desktop build: bodyclock_core plus the CLI harness, into build\.
rem
rem The point of a desktop build is that the core can be exercised without a
rem device -- see cli\main.cpp. It is not a shipping target.
rem
rem Usage: build.cmd [clean] [asr]
rem
rem   asr   also build speech recognition, which needs ggml. Assembles a desktop
rem         ggml SDK into build\llama-sdk\ from a llama.cpp checkout and
rem         configures with -DBODYCLOCK_ASR=ON. Only ggml is built -- recognition
rem         does not link llama -- so this is a couple of minutes, not the twenty
rem         the full engine takes.
rem
rem Env overrides:
rem   VS_DIR       Visual Studio install root
rem   VCPKG_ROOT   vcpkg checkout (default: the VS-bundled one)
rem   NINJA        ninja.exe path
rem   LLAMA_SRC    llama.cpp checkout (default: a sibling of the repo), `asr` only
setlocal EnableExtensions EnableDelayedExpansion

set "PROJECT_DIR=%~dp0"
set "BUILD_DIR=%PROJECT_DIR%build"

set "DO_CLEAN="
set "DO_ASR="
:args
if "%~1"=="" goto args_done
if /I "%~1"=="clean" ( set "DO_CLEAN=1" & shift & goto args )
if /I "%~1"=="asr"   ( set "DO_ASR=1"   & shift & goto args )
echo Unknown argument "%~1". Usage: build.cmd [clean] [asr]>&2
exit /b 1
:args_done

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
rem A desktop ggml SDK, shaped like the one harmony\build.cmd assembles per ABI:
rem include\ plus lib\. Recognition links ggml and nothing else, so only the three
rem ggml targets are built -- libllama is not needed and is not asked for.
rem -----------------------------------------------------------------------------
:ggml_sdk
if not defined LLAMA_SRC for %%I in ("%PROJECT_DIR%..") do set "LLAMA_SRC=%%~fI\llama.cpp"
if not exist "!LLAMA_SRC!\ggml\include\ggml.h" (
    echo no llama.cpp at "!LLAMA_SRC!".>&2
    echo clone it, or point LLAMA_SRC at an existing checkout:>&2
    echo   git clone --depth 1 https://github.com/ggml-org/llama.cpp "!LLAMA_SRC!">&2
    exit /b 1
)

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
