@echo off
rem Desktop build: bodyclock_core plus the CLI harness, into build\.
rem
rem The point of a desktop build is that the core can be exercised without a
rem device -- see cli\main.cpp. It is not a shipping target.
rem
rem Usage: build.cmd [clean]
rem Env overrides:
rem   VS_DIR       Visual Studio install root
rem   VCPKG_ROOT   vcpkg checkout (default: the VS-bundled one)
rem   NINJA        ninja.exe path
setlocal EnableExtensions

set "PROJECT_DIR=%~dp0"
set "BUILD_DIR=%PROJECT_DIR%build"

if /I "%~1"=="clean" if exist "%BUILD_DIR%" rd /s /q "%BUILD_DIR%"

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

if not exist "%BUILD_DIR%\build.ninja" (
    cmake -S "%PROJECT_DIR%." -B "%BUILD_DIR%" ^
        -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
        -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
        -DVCPKG_TARGET_TRIPLET=x64-windows ^
        -DVCPKG_INSTALLED_DIR="%PROJECT_DIR%vcpkg_installed"
    if errorlevel 1 exit /b 1
)

cmake --build "%BUILD_DIR%" --config Release
rem Propagate cmake's exit code. Bare `exit /b` resets ERRORLEVEL to 0 despite
rem the folklore, so a failed build would report success.
exit /b %errorlevel%
