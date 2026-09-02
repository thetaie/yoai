@echo off
rem Cross-compile the core's native dependencies for HarmonyOS / OpenHarmony and
rem lay them out under harmony\prebuilt\<ABI>\ so entry\src\main\cpp\CMakeLists.txt
rem (which passes -DCMAKE_PREFIX_PATH) and the repo-root CMakeLists.txt
rem find_package() calls resolve.
rem
rem vcpkg ships OHOS triplets but has taught no *port* about the platform, and the
rem pinned tool does not map CMAKE_SYSTEM_NAME=OHOS to its own toolchain. Both are
rem worked around by the overlay triplets in harmony\vcpkg-triplets\ -- read the
rem comments there before changing anything here.
rem
rem Re-running is idempotent: vcpkg skips already-built ports and the destination
rem is refreshed.
rem
rem Usage:  build-prebuilt.cmd [abi]
rem   abi defaults to arm64-v8a; one of arm64-v8a^|x86_64^|armeabi-v7a.
rem Env overrides:
rem   VCPKG_ROOT      vcpkg checkout (default: a sibling of the repo)
rem   OHOS_SDK_ROOT   the SDK dir CONTAINING native\ (default: resolved from DevEco)
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "ABI=%~1"
if "%ABI%"=="" set "ABI=arm64-v8a"

rem READ from vcpkg.json's "builtin-baseline" rather than repeating the hash here:
rem two copies of a pin is one copy that silently goes stale.
rem tokens=4 delims=" splits `  "builtin-baseline": "<hash>",` into ["  "]
rem ["builtin-baseline"][": "][<hash>], so token 4 is the hash.
set "VCPKG_JSON=%~dp0..\vcpkg.json"
set "BASELINE="
for /f tokens^=4^ delims^=^" %%A in ('findstr /c:"builtin-baseline" "%VCPKG_JSON%"') do set "BASELINE=%%A"
if not defined BASELINE (
    echo no "builtin-baseline" in "%VCPKG_JSON%">&2
    exit /b 1
)

rem OHOS ABI -> overlay triplet (harmony\vcpkg-triplets\).
if /I "%ABI%"=="arm64-v8a"   ( set "TRIPLET=arm64-ohos" ) else ^
if /I "%ABI%"=="x86_64"      ( set "TRIPLET=x64-ohos"   ) else ^
if /I "%ABI%"=="armeabi-v7a" ( set "TRIPLET=arm-ohos"   ) else (
    echo abi must be arm64-v8a^|x86_64^|armeabi-v7a ^(got "%ABI%"^)>&2
    exit /b 2
)

rem Default the vcpkg checkout to a sibling of the repo so it is not committed.
if not defined VCPKG_ROOT for %%I in ("%~dp0..\..") do set "VCPKG_ROOT=%%~fI\vcpkg"

rem --- Resolve the OpenHarmony SDK ---------------------------------------------
rem OHOS_SDK_ROOT must name the directory CONTAINING native\ (vcpkg's
rem scripts\toolchains\ohos.cmake appends \native itself).
if not defined OHOS_SDK_ROOT (
    for %%D in (
        "%DEVECO_HOME%\sdk\default\openharmony"
        "%ProgramFiles%\Huawei\DevEco Studio\sdk\default\openharmony"
        "%LOCALAPPDATA%\OpenHarmony\Sdk\default\openharmony"
    ) do (
        if not defined OHOS_SDK_ROOT if exist "%%~D\native\build\cmake\ohos.toolchain.cmake" set "OHOS_SDK_ROOT=%%~D"
    )
)
if not defined OHOS_SDK_ROOT (
    echo OpenHarmony SDK not found. Set OHOS_SDK_ROOT to the directory containing native\.>&2
    exit /b 1
)
if not exist "%OHOS_SDK_ROOT%\native\build\cmake\ohos.toolchain.cmake" (
    echo No native\build\cmake\ohos.toolchain.cmake under "%OHOS_SDK_ROOT%".>&2
    exit /b 1
)

rem --- Space-free SDK path (mandatory) -----------------------------------------
rem OpenSSL's generated Makefile hands the compiler path and --sysroot to /bin/sh
rem UNQUOTED, so a space anywhere in the SDK path breaks the build with
rem "/bin/sh: .../Huawei/DevEco: No such file or directory". A directory junction
rem is enough to work around it. Detect a space via substitution: !VAR: =! strips
rem spaces, so a differing result means VAR had one.
set "SDK_NOSP=!OHOS_SDK_ROOT: =!"
if not "!SDK_NOSP!"=="!OHOS_SDK_ROOT!" (
    set "VR_NOSP=!VCPKG_ROOT: =!"
    if not "!VR_NOSP!"=="!VCPKG_ROOT!" (
        echo SDK path contains a space ^["!OHOS_SDK_ROOT!"^] and so does VCPKG_ROOT;>&2
        echo point OHOS_SDK_ROOT at an SDK under a space-free path.>&2
        exit /b 1
    )
    set "SDK_LINK=!VCPKG_ROOT!\ohos-sdk"
    if not exist "!SDK_LINK!\native\build\cmake\ohos.toolchain.cmake" (
        echo SDK path has a space; linking a space-free alias: !SDK_LINK!
        if exist "!SDK_LINK!" rmdir "!SDK_LINK!" 2>nul
        mklink /J "!SDK_LINK!" "!OHOS_SDK_ROOT!" >nul
        if errorlevel 1 (
            echo could not create the junction ^(mklink /J^)>&2
            exit /b 1
        )
    )
    set "OHOS_SDK_ROOT=!SDK_LINK!"
)

echo ABI=%ABI%  triplet=!TRIPLET!
echo SDK=!OHOS_SDK_ROOT!
echo vcpkg=!VCPKG_ROOT!
echo baseline=!BASELINE!

rem --- Bootstrap vcpkg at the pinned baseline -----------------------------------
if not exist "!VCPKG_ROOT!\.git" (
    echo Cloning vcpkg ...
    git clone https://github.com/microsoft/vcpkg "!VCPKG_ROOT!"
    if errorlevel 1 exit /b 1
)
git -C "!VCPKG_ROOT!" checkout %BASELINE%
if errorlevel 1 exit /b 1
if not exist "!VCPKG_ROOT!\vcpkg.exe" call "!VCPKG_ROOT!\bootstrap-vcpkg.bat" -disableMetrics

rem --- Build the core's deps for OHOS (classic mode) ----------------------------
rem --classic: this script runs inside the repo, whose vcpkg.json would otherwise
rem put vcpkg into manifest mode (which rejects port-name arguments).
rem
rem zlib is named explicitly even though curl pulls it in: the OHOS triplet builds
rem everything static, so the final link needs it resolvable on its own.
set "PORTS=openssl curl[openssl] rapidjson zlib"
echo Installing: !PORTS!  (--triplet !TRIPLET!)
"!VCPKG_ROOT!\vcpkg.exe" install !PORTS! --classic --triplet !TRIPLET! ^
    --overlay-triplets="%~dp0vcpkg-triplets" ^
    --x-buildtrees-root "!VCPKG_ROOT!\buildtrees"
if errorlevel 1 exit /b 1

rem --- Lay out under harmony\prebuilt\<ABI> -------------------------------------
set "INSTALLED=!VCPKG_ROOT!\installed\!TRIPLET!"
if not exist "!INSTALLED!" (
    echo expected install tree missing: !INSTALLED!>&2
    exit /b 1
)
set "DEST=%~dp0prebuilt\%ABI%"
if exist "!DEST!" rmdir /s /q "!DEST!"
mkdir "!DEST!"
rem Copy debug\ too: vcpkg's *Targets.cmake reference both release (lib\) and debug
rem (debug\lib\) imported locations, and CMake verifies both exist at
rem find_package time.
for %%S in (include lib share debug) do (
    if exist "!INSTALLED!\%%S" (
        robocopy "!INSTALLED!\%%S" "!DEST!\%%S" /E /NFL /NDL /NJH /NJS >nul
        rem robocopy exits 0-7 on success, >=8 on failure.
        if errorlevel 8 (
            echo robocopy %%S failed>&2
            exit /b 1
        )
    )
)

echo.
echo Done. Prebuilt deps for %ABI% -^> !DEST!
echo Now build the app in DevEco, or the native target directly:
echo   cmake -B build-ohos -G Ninja ^^
echo     -DCMAKE_TOOLCHAIN_FILE=!OHOS_SDK_ROOT!/native/build/cmake/ohos.toolchain.cmake ^^
echo     -DOHOS_ARCH=%ABI% ^^
echo     -DCMAKE_FIND_ROOT_PATH=!DEST! -DCMAKE_PREFIX_PATH=!DEST! ^^
echo     -DYOAI_BUILD_CLI=OFF
echo   cmake --build build-ohos --target yoai_core
exit /b 0
