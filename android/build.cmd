@echo off
rem ---------------------------------------------------------------------------
rem Everything the Android app needs built natively, before Gradle can compile
rem the APK. This does NOT build the app itself -- `gradlew assembleDebug`, or
rem open android\ in Android Studio, for that. It builds what the app links
rem against.
rem
rem THIS IS THE WINDOWS SCRIPT. build.sh is the Linux and macOS one, and the two
rem are kept in agreement by hand. That is a cost, and it replaced a worse one:
rem this file used to be a four-line wrapper that ran build.sh through bash, and
rem `where bash` on Windows answers C:\Windows\System32\bash.exe -- the WSL
rem launcher -- because System32 is on every PATH. WSL cannot open a D:\ path, so
rem the wrapper failed with
rem
rem   /bin/bash: D:\projects\yoai\android\build.sh: No such file or directory
rem
rem which reads as a missing file and is not one. A second native script is two
rem things to keep in step; one that works only when the right bash happens to
rem come first on PATH is not a script, it is a coin flip.
rem
rem   build.cmd [abi] [deps^|llama^|qtts^|vulkan^|apk^|all] [clean]
rem             [arch=<march>^|baseline]
rem
rem     abi     arm64-v8a (default) ^| x86_64
rem     deps    the core's C++ dependencies via vcpkg -> prebuilt\<abi>\
rem             (REQUIRED: without it the native module fails to configure on
rem             its first find_package)
rem     llama   the on-device LLM engine, and the ggml recognition rides on
rem             -> prebuilt\llama-sdk\<abi>\
rem     qtts    speech output (qwentts.cpp) -> prebuilt\qtts-sdk\<abi>\
rem             Needs a llama.cpp checkout too, whether or not `llama` is being
rem             built: it compiles llama.cpp's ggml. See :qtts for why.
rem     vulkan  modifies qtts: also build ggml's Vulkan backend, for Adreno.
rem             Implies qtts. Not a target of its own.
rem     apk     assemble the debug APK via the Gradle wrapper
rem     all     deps, llama, apk -- and what a bare `build.cmd` does
rem     clean   discard the cached cross-build trees and SDKs first
rem     arch=   llama/qtts only: the -march feature set; `baseline` means armv8-a
rem
rem **About arch=.** design/voice.md measures a cross build with no -march
rem landing on baseline armv8-a and costing 2.9x prefill and 2.2x decode, because
rem CMake's feature probes compile AND RUN a test binary and cannot run one for
rem the phone. So the feature set is asserted here rather than detected. The
rem default below is the HarmonyOS lane's, which is what a Kirin 9020 has; a
rem current Qualcomm core is armv9.2-a and has more. The Device panel in the app
rem prints what the chip actually reports, which is how the assertion gets
rem checked -- set arch= to match it, and re-measure.
rem
rem Env overrides:
rem   ANDROID_SDK_ROOT  the SDK. Default: ANDROID_HOME, then sdk.dir in
rem                     android\local.properties
rem   ANDROID_NDK_ROOT  the NDK (default: the version in android\gradle.properties)
rem   VCPKG_ROOT        vcpkg checkout (default: a sibling of the repo)
rem   LLAMA_SRC         llama.cpp checkout (default: a sibling of the repo)
rem   QTTS_SRC          qwentts.cpp checkout (default: a sibling of the repo)
rem
rem `..\third\sync.cmd clone` fetches the checkouts at their pinned commits, with
rem the patches this script expects already applied.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"
set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
for %%I in ("%SCRIPT_DIR%\..") do set "REPO_ROOT=%%~fI"

rem --- Arguments, position-independent ------------------------------------------
rem An unrecognized token is an ERROR rather than a guess, for the reason
rem harmony/build.sh records: its predecessor treated any unknown token as the
rem -march string, so a mistyped target silently became `-march=lama`.
set "ABI="
set "DO_DEPS="
set "DO_LLAMA="
set "DO_QTTS="
set "DO_APK="
set "QTTS_VULKAN="
set "ARCH="
set "ARCH_SET="
set "CLEAN="

:args
if "%~1"=="" goto args_done
set "TOK=%~1"
if /I "!TOK!"=="deps"     ( set "DO_DEPS=1"  & goto args_next )
if /I "!TOK!"=="llama"    ( set "DO_LLAMA=1" & goto args_next )
if /I "!TOK!"=="qtts"     ( set "DO_QTTS=1"  & goto args_next )
if /I "!TOK!"=="vulkan"   ( set "DO_QTTS=1"  & set "QTTS_VULKAN=1" & goto args_next )
if /I "!TOK!"=="apk"      ( set "DO_APK=1"   & goto args_next )
if /I "!TOK!"=="all"      ( set "DO_DEPS=1"  & set "DO_LLAMA=1" & set "DO_APK=1" & goto args_next )
if /I "!TOK!"=="clean"    ( set "CLEAN=1"    & goto args_next )
if /I "!TOK!"=="baseline" ( set "ARCH="      & set "ARCH_SET=1" & goto args_next )
if /I "!TOK!"=="-h"       ( goto usage )
if /I "!TOK!"=="--help"   ( goto usage )
if /I "!TOK!"=="/?"       ( goto usage )
rem `arch=<march>` arrives as TWO tokens: cmd's tokenizer treats `=` as an
rem argument separator, so an unquoted `arch=armv8.6-a+i8mm` splits into "arch"
rem and "armv8.6-a+i8mm". Reassembling it here means the same command line works
rem unquoted on this script and on build.sh, which is the point of them taking
rem the same tokens at all. The quoted form ("arch=...") stays valid too.
if /I "!TOK!"=="arch"       goto arg_arch
if /I "!TOK:~0,5!"=="arch=" ( set "ARCH=!TOK:~5!" & set "ARCH_SET=1" & goto args_next )
if not defined ABI ( set "ABI=!TOK!" & goto args_next )
echo [build] unknown argument "!TOK!". Try: build.cmd --help>&2
exit /b 2

rem Flat, with labels, rather than a nested if-block: harmony\build.cmd records
rem that an `exit /b <n>` two paren levels deep printed its message and then
rem reported SUCCESS, so a usage error looked like a completed build.
:arg_arch
if "%~2"=="" goto arch_no_value
set "ARCH=%~2"
set "ARCH_SET=1"
shift
goto args_next

:arch_no_value
echo [build] arch needs a value, e.g. arch=armv8.6-a+i8mm+dotprod - or the token baseline>&2
exit /b 2

:args_next
shift
goto args

:usage
rem The header above is the help text, printed by stripping the `rem ` prefix.
for /f "usebackq tokens=1,* delims=:" %%A in (`findstr /n "^" "%~f0"`) do (
    if %%A GTR 2 (
        set "LINE=%%B"
        if "!LINE!"=="rem ---------------------------------------------------------------------------" goto :eof
        if "!LINE:~0,4!"=="rem " ( echo(!LINE:~4! ) else ( echo( )
    )
)
goto :eof

:args_done
if not defined ABI set "ABI=arm64-v8a"
rem NOTHING named means none of the five. harmony\build.cmd carried this guard
rem testing only three of its targets, so naming one it had left out read as an
rem empty command line and quietly built everything.
if not defined DO_DEPS if not defined DO_LLAMA if not defined DO_QTTS ^
if not defined DO_APK (
    set "DO_DEPS=1"
    set "DO_LLAMA=1"
    set "DO_APK=1"
)

rem The Kirin 9020's feature set, which is where every number in design/voice.md
rem comes from. Kept as the default so a build on a new device is comparable
rem against those before it is optimised for the new one.
if not defined ARCH_SET set "ARCH=armv8.6-a+i8mm+bf16+dotprod+fp16"

if /I "%ABI%"=="arm64-v8a" (
    set "VCPKG_TRIPLET=arm64-android"
    set "ANDROID_ABI_CMAKE=arm64-v8a"
) else if /I "%ABI%"=="x86_64" (
    set "VCPKG_TRIPLET=x64-android"
    set "ANDROID_ABI_CMAKE=x86_64"
    set "ARCH="
) else (
    echo [build] unsupported abi "%ABI%" ^(arm64-v8a, x86_64^)>&2
    exit /b 2
)

rem The API level the NDK targets. 26 matches minSdk in app\build.gradle.kts; a
rem mismatch is a link error naming a libc symbol rather than a configuration.
set "API_LEVEL=26"

rem ===========================================================================
rem Toolchain discovery
rem ===========================================================================
rem Three sources, in the order of how explicitly each one was chosen.
rem ANDROID_HOME is here because it is the variable the SDK's own installers and
rem Android Studio still set -- looking only at ANDROID_SDK_ROOT means a machine
rem with a perfectly good SDK reports not having one.
if not defined ANDROID_SDK_ROOT if defined ANDROID_HOME set "ANDROID_SDK_ROOT=%ANDROID_HOME%"
if not defined ANDROID_SDK_ROOT if exist "%SCRIPT_DIR%\local.properties" (
    rem local.properties is the file Gradle already reads, so there is one place
    rem the SDK path lives rather than two that can disagree. Gradle escapes what
    rem it writes there -- `C\:\\Users\\me\\...` -- so it is unescaped below.
    for /f "usebackq tokens=1,* delims==" %%A in (`findstr /b /c:"sdk.dir=" "%SCRIPT_DIR%\local.properties"`) do (
        set "SDKDIR=%%B"
    )
    if defined SDKDIR (
        set "SDKDIR=!SDKDIR:\\=\!"
        set "SDKDIR=!SDKDIR:\:=:!"
        set "ANDROID_SDK_ROOT=!SDKDIR!"
    )
)
if not defined ANDROID_SDK_ROOT goto no_sdk
if not exist "!ANDROID_SDK_ROOT!\" goto no_sdk

if not defined ANDROID_NDK_ROOT (
    set "NDK_VER="
    if exist "%SCRIPT_DIR%\gradle.properties" (
        for /f "usebackq tokens=1,* delims==" %%A in (`findstr /b /c:"yoai.ndk=" "%SCRIPT_DIR%\gradle.properties"`) do set "NDK_VER=%%B"
    )
    if not defined NDK_VER (
        echo [build] no yoai.ndk in "%SCRIPT_DIR%\gradle.properties">&2
        exit /b 1
    )
    set "ANDROID_NDK_ROOT=!ANDROID_SDK_ROOT!\ndk\!NDK_VER!"
)
if not exist "!ANDROID_NDK_ROOT!\" (
    echo [build] no NDK at "!ANDROID_NDK_ROOT!">&2
    exit /b 1
)

set "NDK_TOOLCHAIN=!ANDROID_NDK_ROOT!\build\cmake\android.toolchain.cmake"
if not exist "!NDK_TOOLCHAIN!" (
    echo [build] no toolchain file at "!NDK_TOOLCHAIN!">&2
    exit /b 1
)

rem The SDK's cmake and ninja, so the version is the one app\build.gradle.kts
rem pins rather than whatever is on PATH.
set "CMAKE_BIN="
set "NINJA_BIN="
for /d %%D in ("!ANDROID_SDK_ROOT!\cmake\*") do (
    if not defined CMAKE_BIN if exist "%%~fD\bin\cmake.exe" set "CMAKE_BIN=%%~fD\bin\cmake.exe"
    if not defined NINJA_BIN if exist "%%~fD\bin\ninja.exe" set "NINJA_BIN=%%~fD\bin\ninja.exe"
)
if not defined CMAKE_BIN for %%P in (cmake.exe) do set "CMAKE_BIN=%%~$PATH:P"
if not defined NINJA_BIN for %%P in (ninja.exe) do set "NINJA_BIN=%%~$PATH:P"
if not defined CMAKE_BIN (
    echo [build] need cmake ^(the SDK's cmake\*\bin, or on PATH^)>&2
    exit /b 1
)
if not defined NINJA_BIN (
    echo [build] need ninja ^(the SDK's cmake\*\bin, or on PATH^)>&2
    exit /b 1
)

if not defined VCPKG_ROOT for %%I in ("%REPO_ROOT%\..") do set "VCPKG_ROOT=%%~fI\vcpkg"
if not defined LLAMA_SRC  for %%I in ("%REPO_ROOT%\..") do set "LLAMA_SRC=%%~fI\llama.cpp"
if not defined QTTS_SRC   for %%I in ("%REPO_ROOT%\..") do set "QTTS_SRC=%%~fI\qwentts.cpp"

if defined ARCH ( set "ARCH_SHOWN=!ARCH!" ) else ( set "ARCH_SHOWN=baseline" )
echo [build] abi=%ABI%  api=%API_LEVEL%  arch=!ARCH_SHOWN!
echo [build] ndk=!ANDROID_NDK_ROOT!

if defined CLEAN (
    echo [build] cleaning cross-build trees
    if exist "%SCRIPT_DIR%\.llama-build\%ABI%"      rmdir /s /q "%SCRIPT_DIR%\.llama-build\%ABI%"
    if exist "%SCRIPT_DIR%\.qtts-build\%ABI%"       rmdir /s /q "%SCRIPT_DIR%\.qtts-build\%ABI%"
    if exist "%SCRIPT_DIR%\prebuilt\llama-sdk\%ABI%" rmdir /s /q "%SCRIPT_DIR%\prebuilt\llama-sdk\%ABI%"
    if exist "%SCRIPT_DIR%\prebuilt\qtts-sdk\%ABI%"  rmdir /s /q "%SCRIPT_DIR%\prebuilt\qtts-sdk\%ABI%"
)

rem Shared by both engine builds. ANDROID_ABI/PLATFORM are what the NDK
rem toolchain file reads; the rest is this project's policy.
rem
rem ANDROID_STL matches app\build.gradle.kts rather than taking the NDK's default
rem of c++_static. For static archives the two are nearly the same thing -- an .a
rem carries undefined references to libc++ either way, and it is the final shared
rem library that decides which one resolves them -- so this is about not having
rem to re-derive that every time the question comes up.
set "NDK_CFG=-G Ninja"
set "NDK_CFG=!NDK_CFG! -DCMAKE_MAKE_PROGRAM="!NINJA_BIN!""
set "NDK_CFG=!NDK_CFG! -DCMAKE_TOOLCHAIN_FILE="!NDK_TOOLCHAIN!""
set "NDK_CFG=!NDK_CFG! -DANDROID_ABI=!ANDROID_ABI_CMAKE!"
set "NDK_CFG=!NDK_CFG! -DANDROID_PLATFORM=android-%API_LEVEL%"
set "NDK_CFG=!NDK_CFG! -DANDROID_STL=c++_shared"
set "NDK_CFG=!NDK_CFG! -DCMAKE_BUILD_TYPE=Release"

if defined DO_DEPS  ( call :deps  || exit /b 1 )
if defined DO_LLAMA ( call :llama || exit /b 1 )
if defined DO_QTTS  ( call :qtts  || exit /b 1 )
if defined DO_APK   ( call :apk   || exit /b 1 )
echo.
echo [build] done.
exit /b 0

:no_sdk
echo [build] no Android SDK at "%ANDROID_SDK_ROOT%".>&2
echo [build] set ANDROID_HOME or ANDROID_SDK_ROOT, or put sdk.dir in>&2
echo [build]   %SCRIPT_DIR%\local.properties>&2
exit /b 1

rem ===========================================================================
rem deps -- the core's own dependencies, via vcpkg
rem ===========================================================================
:deps
rem READ from vcpkg.json's "builtin-baseline" rather than repeating the hash
rem here: two copies of a pin is one copy that silently goes stale.
rem tokens=4 delims=" splits `  "builtin-baseline": "<hash>",` into ["  "]
rem ["builtin-baseline"][": "][<hash>], so token 4 is the hash.
set "VCPKG_JSON=%REPO_ROOT%\vcpkg.json"
set "BASELINE="
for /f tokens^=4^ delims^=^" %%A in ('findstr /c:"builtin-baseline" "%VCPKG_JSON%"') do set "BASELINE=%%A"
if not defined BASELINE (
    echo [deps] no "builtin-baseline" in "%VCPKG_JSON%">&2
    exit /b 1
)

echo [deps] triplet=!VCPKG_TRIPLET!  vcpkg=!VCPKG_ROOT!  baseline=!BASELINE!

if not exist "!VCPKG_ROOT!\.git" (
    echo [deps] cloning vcpkg ...
    git clone https://github.com/microsoft/vcpkg "!VCPKG_ROOT!" || exit /b 1
)
git -C "!VCPKG_ROOT!" checkout !BASELINE! || exit /b 1
if not exist "!VCPKG_ROOT!\vcpkg.exe" call "!VCPKG_ROOT!\bootstrap-vcpkg.bat" -disableMetrics

rem No overlay triplet, unlike the OHOS lane: vcpkg ships arm64-android and
rem x64-android, and they are already static.
set "ANDROID_NDK_HOME=!ANDROID_NDK_ROOT!"

rem Manifest mode, from the repository's vcpkg.json. The dependency list lives
rem there and nowhere else, so a port the core starts to need is added once and
rem every lane installs it. This used to be --classic with the ports named
rem here: a second list, which the desktop build never read.
rem
rem The install root is per triplet rather than VCPKG_ROOT\installed. A manifest
rem install may remove what its manifest does not ask for, and this vcpkg
rem checkout is shared by every ABI of both mobile lanes.
set "VCPKG_INSTALL_ROOT=!VCPKG_ROOT!\installed-!VCPKG_TRIPLET!"
echo [deps] installing from vcpkg.json into !VCPKG_INSTALL_ROOT!
"!VCPKG_ROOT!\vcpkg.exe" install --triplet !VCPKG_TRIPLET! ^
    --x-manifest-root="%REPO_ROOT%" ^
    --x-install-root="!VCPKG_INSTALL_ROOT!" ^
    --x-buildtrees-root "!VCPKG_ROOT!\buildtrees" || exit /b 1

set "INSTALLED=!VCPKG_INSTALL_ROOT!\!VCPKG_TRIPLET!"
if not exist "!INSTALLED!\" (
    echo [deps] expected install tree missing: "!INSTALLED!">&2
    exit /b 1
)

set "DEST=%SCRIPT_DIR%\prebuilt\%ABI%"
if exist "!DEST!" rmdir /s /q "!DEST!"
mkdir "!DEST!"
rem debug\ too: vcpkg's *Targets.cmake reference both release (lib\) and debug
rem (debug\lib\) imported locations, and CMake verifies both exist at
rem find_package time.
for %%S in (include lib share debug) do (
    if exist "!INSTALLED!\%%S" xcopy /e /i /q /y "!INSTALLED!\%%S" "!DEST!\%%S" >nul
)
echo [deps] done -^> !DEST!
exit /b 0

rem ===========================================================================
rem llama -- the on-device LLM, and the ggml that recognition links
rem ===========================================================================
:llama
if not exist "!LLAMA_SRC!\" (
    echo [llama] no llama.cpp at "!LLAMA_SRC!".>&2
    echo [llama]   ..\third\sync.cmd clone llama.cpp   ^(pinned commit, patch applied^)>&2
    exit /b 1
)

set "LBUILD=%SCRIPT_DIR%\.llama-build\%ABI%"
set "LSDK=%SCRIPT_DIR%\prebuilt\llama-sdk\%ABI%"

rem GGML_OPENMP=OFF, and this is not just a link fix.
rem
rem ggml turns OpenMP on wherever find_package(OpenMP) succeeds. It fails on the
rem OHOS sysroot, which has no libomp, so every figure in design/voice.md was
rem taken on **ggml's own thread pool**. The NDK does ship libomp, so the same
rem source would quietly run a different scheduler here and a thread-count sweep
rem would not be comparable with the Kirin's -- which is most of what this lane
rem exists to compare.
rem
rem It also fails to link if left on: the archives reference __kmpc_* and nothing
rem pulls the runtime into libyoai.so.
set "LCFG=!NDK_CFG! -S "!LLAMA_SRC!" -B "!LBUILD!""
set "LCFG=!LCFG! -DBUILD_SHARED_LIBS=OFF"
set "LCFG=!LCFG! -DLLAMA_CURL=OFF"
set "LCFG=!LCFG! -DLLAMA_BUILD_TESTS=OFF"
set "LCFG=!LCFG! -DLLAMA_BUILD_EXAMPLES=OFF"
set "LCFG=!LCFG! -DLLAMA_BUILD_SERVER=OFF"
set "LCFG=!LCFG! -DLLAMA_BUILD_TOOLS=OFF"
set "LCFG=!LCFG! -DGGML_OPENMP=OFF"
if defined ARCH set "LCFG=!LCFG! -DGGML_CPU_ARM_ARCH=!ARCH!"

"!CMAKE_BIN!" !LCFG! || exit /b 1
rem Only the libraries: upstream's CLI target links implementation libs the OFF
rem switches above remove, so asking for it would fail the link.
"!CMAKE_BIN!" --build "!LBUILD!" --target llama ggml ggml-cpu ggml-base || exit /b 1

echo [llama] assembling !LSDK!
if not exist "!LSDK!\include" mkdir "!LSDK!\include"
if not exist "!LSDK!\lib"     mkdir "!LSDK!\lib"
copy /y "!LLAMA_SRC!\include\llama.h"    "!LSDK!\include\" >nul
copy /y "!LLAMA_SRC!\ggml\include\*.h"   "!LSDK!\include\" >nul
rem Every archive is checked before it is copied, so a partial SDK stops the
rem script rather than being discovered later as a link error.
for %%L in ("!LBUILD!\src\libllama.a"
            "!LBUILD!\ggml\src\libggml.a"
            "!LBUILD!\ggml\src\libggml-cpu.a"
            "!LBUILD!\ggml\src\libggml-base.a") do (
    if not exist "%%~L" (
        echo [llama] missing %%~L>&2
        exit /b 1
    )
    copy /y "%%~L" "!LSDK!\lib\" >nul
)
echo [llama] done -^> !LSDK!
exit /b 0

rem ===========================================================================
rem qtts -- speech output (qwentts.cpp), optionally with ggml's Vulkan backend
rem ===========================================================================
:qtts
if not exist "!QTTS_SRC!\" (
    echo [qtts] no qwentts.cpp at "!QTTS_SRC!".>&2
    echo [qtts]   ..\third\sync.cmd clone qwentts.cpp>&2
    exit /b 1
)

rem third\patches\ has two for this checkout. qwentts-ggml-ohos-affinity.patch is
rem for musl's missing pthread_setaffinity_np and does NOT apply here -- bionic
rem has it. The thread-count one is not platform-specific: qwentts.cpp picks
rem hardware_concurrency()/2 reasoning about hyperthreads a phone does not have.
rem It is applied through the SAME patch file as the other lanes, so they cannot
rem drift into overriding the thread count differently.
call :qpatch "!QTTS_SRC!" qwentts-thread-count-override.patch || exit /b 1

set "QBUILD=%SCRIPT_DIR%\.qtts-build\%ABI%"
set "QSDK=%SCRIPT_DIR%\prebuilt\qtts-sdk\%ABI%"

rem ONE ggml for both lanes, and llama.cpp's is the one. This used to compile
rem qwentts's own ggml submodule and the app linked that copy for everything,
rem because it superset llama's. The 2026-09-21 update inverted it: libllama
rem wants ggml_dsv4_hc_pre_gated, which the fork does not have, so whichever copy
rem is kept one symbol goes unmet and the app does not link. GGML_SOURCE_DIR,
rem added upstream the same day, points the qwentts build at any ggml tree -- so
rem both lanes compile the same one from the same commit. The cost is the fork's
rem fused snake kernel for the DAC decoder; see design/voice.md. This target
rem therefore needs a llama.cpp checkout even when `llama` is not being built.
if not exist "!LLAMA_SRC!\ggml\CMakeLists.txt" (
    echo [qtts] qtts compiles llama.cpp's ggml and there is none at "!LLAMA_SRC!\ggml".>&2
    echo [qtts]   ..\third\sync.cmd clone llama.cpp>&2
    exit /b 1
)
echo [qtts] ggml=!LLAMA_SRC!\ggml

set "QCFG=!NDK_CFG! -S "!QTTS_SRC!" -B "!QBUILD!""
set "QCFG=!QCFG! -DGGML_SOURCE_DIR="!LLAMA_SRC:\=/!/ggml""
set "QCFG=!QCFG! -DBUILD_SHARED_LIBS=OFF"
rem Same reason as the llama lane's: this ggml has to schedule threads the way
rem the one design/voice.md measured did.
set "QCFG=!QCFG! -DGGML_OPENMP=OFF"
if defined ARCH set "QCFG=!QCFG! -DGGML_CPU_ARM_ARCH=!ARCH!"
if defined QTTS_VULKAN (
    rem The reason this is worth a target at all: design/voice.md measures ggml's
    rem Vulkan backend at RTF 0.60 on a laptop iGPU against 1.13 on its CPU, and
    rem the HarmonyOS attempt died to a platform policy (GPU_LEAK_KILL) rather
    rem than to anything about the backend. Android has no such policy, so Adreno
    rem is the first real chance to find out.
    set "QCFG=!QCFG! -DGGML_VULKAN=ON"
)

"!CMAKE_BIN!" !QCFG! || exit /b 1
"!CMAKE_BIN!" --build "!QBUILD!" || exit /b 1

echo [qtts] assembling !QSDK!
if not exist "!QSDK!\include" mkdir "!QSDK!\include"
if not exist "!QSDK!\lib"     mkdir "!QSDK!\lib"
rem qwen.h is what android\app\src\main\cpp\CMakeLists.txt probes for, so it is
rem the marker that says this SDK is complete.
copy /y "!QTTS_SRC!\src\qwen*.h" "!QSDK!\include\" >nul
rem From LLAMA_SRC, not QTTS_SRC: the headers have to be the ones the archives
rem below were compiled from, and GGML_SOURCE_DIR moved that.
copy /y "!LLAMA_SRC!\ggml\include\*.h" "!QSDK!\include\" >nul
rem Collected by search rather than by name because ggml splits its archives
rem differently between versions, and the Vulkan backend adds one more.
for /r "%SCRIPT_DIR%\.qtts-build\%ABI%" %%F in (libqwen-core.a libggml*.a) do (
    copy /y "%%~fF" "!QSDK!\lib\" >nul
)
if not exist "!QSDK!\include\qwen.h" (
    echo [qtts] no qwen.h landed in "!QSDK!\include" -- the layout moved>&2
    exit /b 1
)
if not exist "!QSDK!\lib\libqwen-core.a" (
    echo [qtts] qwentts built but libqwen-core.a is not in "!QSDK!\lib">&2
    exit /b 1
)
echo [qtts] done -^> !QSDK!
if defined QTTS_VULKAN echo [qtts] Vulkan backend included; the core links it when libggml-vulkan.a is present
exit /b 0

rem ===========================================================================
rem apk
rem ===========================================================================
:apk
echo [apk] assembleDebug
call "%SCRIPT_DIR%\gradlew.bat" assembleDebug -Pyoai.abi=%ABI% || exit /b 1
echo [apk] done -^> %SCRIPT_DIR%\app\build\outputs\apk\debug\
exit /b 0

rem ===========================================================================
rem Apply one patch from ..\third\patches\ to a checkout, at most once.
rem
rem   %1  the git work tree to apply in
rem   %2  the file name under third\patches\
rem
rem `--reverse --check` is the idempotence test, and a better one than a marker
rem file: it succeeds only when the change is ALREADY present, so a re-run is a
rem no-op, a checkout that moved under the patch fails loudly, and a file
rem somebody edited by hand is detected rather than patched twice.
rem ===========================================================================
:qpatch
set "QP_TREE=%~1"
set "QP_FILE=%REPO_ROOT%\third\patches\%~2"
if not exist "!QP_FILE!" (
    echo [patch] no patch at "!QP_FILE!".>&2
    exit /b 1
)
git -C "!QP_TREE!" apply --reverse --check "!QP_FILE!" >nul 2>&1
if not errorlevel 1 (
    echo [patch] already patched: %~2
    exit /b 0
)
echo [patch] patching: %~2
git -C "!QP_TREE!" apply "!QP_FILE!"
if errorlevel 1 (
    echo [patch] could not apply "!QP_FILE!" in "!QP_TREE!".>&2
    echo [patch] the checkout moved under it; re-read the patch and rebase it.>&2
    exit /b 1
)
exit /b 0
