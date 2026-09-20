@echo off
rem ---------------------------------------------------------------------------
rem Everything the HarmonyOS app needs built natively, before DevEco can compile
rem the HAP. This does NOT build the app itself -- open harmony\ in DevEco for
rem that. It builds what the app links against.
rem
rem   build.cmd [abi] [deps^|llama^|mnn^|hap^|all] [clean] [arch=<march>^|baseline]
rem
rem     abi     arm64-v8a (default) ^| x86_64 ^| armeabi-v7a
rem     deps    the core's C++ dependencies via vcpkg -> prebuilt\<abi>\
rem             (REQUIRED: without it the native module fails to configure on
rem             its first find_package)
rem     llama   the on-device LLM engine  -> prebuilt\llama-sdk\<abi>\
rem             (building it is what turns on-device chat on)
rem     mnn     MNN with the omni Talker -> prebuilt\mnn-sdk\<abi>\
rem             An EVALUATION lane, deliberately NOT part of `all`: nothing in
rem             the app links it yet. See :mnn for what it is for.
rem     hap     compile and sign the app itself, via hvigor. No IDE needed.
rem     all     all three, in that order -- and what a bare `build.cmd` does
rem     clean   discard llama's cached build tree and SDK first. `deps` always
rem             replaces its own output, so there is nothing for it to clean.
rem     arch=   llama only: the -march feature set. `baseline` means plain
rem             armv8-a. See :llama below for why the default is what it is.
rem
rem The abi defaults to arm64-v8a, which is the only one a HarmonyOS NEXT
rem device has -- so it is normally left off entirely. Naming no target means
rem `all`, so the first run is just:
rem
rem     build.cmd
rem
rem and after that, just:
rem
rem     build.cmd hap
rem
rem On-device chat is part of the app rather than an opt-in lane, so the bare
rem command produces an app that has it. That is not free: it needs a llama.cpp
rem checkout (~200 MB, and this script will not clone it for you -- see :llama)
rem plus a few minutes of cross-compile. To build the app WITHOUT the engine,
rem name the two targets you do want:
rem
rem     build.cmd deps hap
rem
rem Env overrides:
rem   DEVECO_HOME     DevEco Studio install (default: read from its own .home
rem                   marker under %LOCALAPPDATA%\Huawei\DevEcoStudio*)
rem   VCPKG_ROOT      vcpkg checkout (default: a sibling of the repo)
rem   OHOS_SDK_ROOT   the SDK dir CONTAINING native\ (default: from DEVECO_HOME)
rem   LLAMA_SRC       llama.cpp checkout (default: a sibling of the repo)
rem   LLAMA_SDK_DIR   where to assemble the llama SDK (default: auto-detected)
rem   MNN_SRC         MNN checkout (default: a sibling of the repo), `mnn` only
rem ---------------------------------------------------------------------------
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem --- Arguments, position-independent ------------------------------------------
rem Every token is a keyword or the ABI, and an unrecognized one is an ERROR
rem rather than a guess. The predecessor treated any unknown token as llama's
rem -march string, so a mistyped target silently became `-march=lama`.
set "ABI="
set "DO_DEPS="
set "DO_LLAMA="
set "DO_MNN="
set "DO_QTTS="
set "QTTS_VULKAN="
set "DO_HAP="
set "ARCH="
set "ARCH_SET="
set "CLEAN="

:args
if "%~1"=="" goto args_done
set "TOK=%~1"
if /I "!TOK!"=="deps"     ( set "DO_DEPS=1"  & goto args_next )
if /I "!TOK!"=="llama"    ( set "DO_LLAMA=1" & goto args_next )
if /I "!TOK!"=="mnn"      ( set "DO_MNN=1"   & goto args_next )
if /I "!TOK!"=="qtts"     ( set "DO_QTTS=1"  & goto args_next )
rem `vulkan` only modifies qtts -- it is not a target of its own, and it implies
rem one, so `build.cmd <abi> vulkan` does the useful thing rather than nothing.
if /I "!TOK!"=="vulkan"   ( set "DO_QTTS=1"  & set "QTTS_VULKAN=1" & goto args_next )
if /I "!TOK!"=="hap"      ( set "DO_HAP=1"   & goto args_next )
if /I "!TOK!"=="all"      ( set "DO_DEPS=1"  & set "DO_LLAMA=1" & set "DO_HAP=1" & goto args_next )
if /I "!TOK!"=="clean"    ( set "CLEAN=1"    & goto args_next )
if /I "!TOK!"=="baseline" ( set "ARCH="      & set "ARCH_SET=1" & goto args_next )
if /I "!TOK!"=="-h"       ( goto usage )
if /I "!TOK!"=="--help"   ( goto usage )
if /I "!TOK!"=="/?"       ( goto usage )
rem `arch=<march>` arrives here as TWO tokens: cmd's own tokenizer treats `=` as
rem an argument separator, so an unquoted `arch=armv8.6-a+i8mm` is split into
rem "arch" and "armv8.6-a+i8mm". Reassembling it here means the same command line
rem works unquoted on both this script and build.sh, which is the point of them
rem taking the same tokens at all. The quoted form ("arch=...") stays valid too,
rem and lands on the second branch.
if /I "!TOK!"=="arch"     goto arg_arch
if /I "!TOK:~0,5!"=="arch=" ( set "ARCH=!TOK:~5!" & set "ARCH_SET=1" & goto args_next )
if not defined ABI ( set "ABI=!TOK!" & goto args_next )
echo unknown argument "!TOK!". Try: build.cmd --help>&2
exit /b 2

rem Flat, with labels, rather than a nested if-block: an `exit /b <n>` inside two
rem levels of parentheses here did print its message and then report SUCCESS, so
rem a usage error looked like a completed build. Not worth diagnosing cmd's
rem parser over -- the flat form has no blocks to mis-nest.
:arg_arch
if "%~2"=="" goto arch_no_value
set "ARCH=%~2"
set "ARCH_SET=1"
shift
goto args_next

:arch_no_value
echo arch needs a value, e.g. arch=armv8.6-a+i8mm+dotprod - or the token baseline>&2
exit /b 2

:args_next
shift
goto args

:args_done
if not defined ABI set "ABI=arm64-v8a"
rem Nothing named: everything, in the order `all` would. The bare command is what
rem a fresh clone runs, and it should end with an installable app that has
rem on-device chat -- not with a half-built tree needing two more invocations
rem nobody mentioned.
set "IMPLIED_ALL="
if not defined DO_DEPS if not defined DO_LLAMA if not defined DO_HAP (
    set "DO_DEPS=1"
    set "DO_LLAMA=1"
    set "DO_HAP=1"
    set "IMPLIED_ALL=1"
)

rem OHOS ABI -> vcpkg overlay triplet (harmony\vcpkg-triplets\). Rejected here
rem rather than at -DOHOS_ARCH, where an unknown ABI fails deep inside the
rem toolchain file with nothing pointing back at the argument.
if /I "%ABI%"=="arm64-v8a"   ( set "TRIPLET=arm64-ohos" ) else ^
if /I "%ABI%"=="x86_64"      ( set "TRIPLET=x64-ohos"   ) else ^
if /I "%ABI%"=="armeabi-v7a" ( set "TRIPLET=arm-ohos"   ) else (
    echo abi must be arm64-v8a^|x86_64^|armeabi-v7a ^(got "%ABI%"^)>&2
    exit /b 2
)
rem Refusing outright is right when llama was ASKED for, and wrong when it was
rem merely implied by a bare command line -- on an ABI that cannot have the
rem engine, `build.cmd armeabi-v7a` should still build everything else. Flat,
rem with a label, for the same reason as :arg_arch above: an `exit /b` two paren
rem levels deep prints its message and then reports success.
if not defined DO_LLAMA goto llama_abi_ok
if /I not "%ABI%"=="armeabi-v7a" goto llama_abi_ok
if defined IMPLIED_ALL (
    echo [build] llama skipped: the on-device engine is built for arm64-v8a^|x86_64 only
    set "DO_LLAMA="
    goto llama_abi_ok
)
echo the on-device engine is built for arm64-v8a^|x86_64 only ^(got "%ABI%"^)>&2
exit /b 2
:llama_abi_ok

rem --- Resolve DevEco Studio ----------------------------------------------------
rem Everything below comes out of the DevEco install: the NDK for the two native
rem halves, and node + hvigor for `hap`.
rem
rem Found by reading the `.home` file DevEco leaves in its per-version config
rem directory, which holds the install path as one line. That beats guessing at
rem Program Files -- this repo's own author installs to D:\Huawei\DevEco Studio,
rem which no hardcoded list would have found.
if not defined DEVECO_HOME (
    for /d %%D in ("%LOCALAPPDATA%\Huawei\DevEcoStudio*") do (
        if exist "%%~D\.home" (
            for /f "usebackq delims=" %%A in ("%%~D\.home") do (
                if exist "%%A\tools" set "DEVECO_HOME=%%A"
            )
        )
    )
)
rem Last resort, for a layout with no .home to read.
if not defined DEVECO_HOME (
    for %%D in (
        "%ProgramFiles%\Huawei\DevEco Studio"
        "%LOCALAPPDATA%\Huawei\DevEco Studio"
    ) do (
        if not defined DEVECO_HOME if exist "%%~D\tools" set "DEVECO_HOME=%%~D"
    )
)

rem --- Resolve the OpenHarmony SDK ----------------------------------------------
rem Both native halves need it, which is the main reason they now live in one
rem script: this block used to exist in four copies and drift between them.
rem
rem OHOS_SDK_ROOT must name the directory CONTAINING native\ (vcpkg's
rem scripts\toolchains\ohos.cmake appends \native itself).
if not defined OHOS_SDK_ROOT (
    for %%D in (
        "%DEVECO_HOME%\sdk\default\openharmony"
        "%LOCALAPPDATA%\OpenHarmony\Sdk\default\openharmony"
    ) do (
        if not defined OHOS_SDK_ROOT if exist "%%~D\native\build\cmake\ohos.toolchain.cmake" set "OHOS_SDK_ROOT=%%~D"
    )
)
if not defined OHOS_SDK_ROOT (
    echo OpenHarmony SDK not found. Set OHOS_SDK_ROOT to the directory containing native\,>&2
    echo or DEVECO_HOME to the DevEco Studio install.>&2
    exit /b 1
)
if not exist "%OHOS_SDK_ROOT%\native\build\cmake\ohos.toolchain.cmake" (
    echo No native\build\cmake\ohos.toolchain.cmake under "%OHOS_SDK_ROOT%".>&2
    exit /b 1
)

rem Default the vcpkg checkout to a sibling of the repo so it is not committed.
if not defined VCPKG_ROOT for %%I in ("%~dp0..\..") do set "VCPKG_ROOT=%%~fI\vcpkg"

echo [build] abi=%ABI%
echo [build] sdk=!OHOS_SDK_ROOT!
if defined DO_DEPS  echo [build] target: deps  ^(vcpkg -^> prebuilt\%ABI%^)
if defined DO_LLAMA echo [build] target: llama ^(llama.cpp -^> prebuilt\llama-sdk\%ABI%^)
if defined DO_MNN   echo [build] target: mnn   ^(MNN -^> prebuilt\mnn-sdk\%ABI%^)
if defined DO_QTTS  echo [build] target: qtts  ^(qwentts.cpp -^> prebuilt\qtts-sdk\%ABI%^)
if defined DO_HAP   echo [build] target: hap   ^(hvigor -^> entry\build\default\outputs^)
echo.

if defined DO_DEPS (
    call :deps
    if errorlevel 1 exit /b 1
)
if defined DO_LLAMA (
    call :llama
    if errorlevel 1 exit /b 1
)
if defined DO_MNN (
    call :mnn
    if errorlevel 1 exit /b 1
)
if defined DO_QTTS (
    call :qtts
    if errorlevel 1 exit /b 1
)

if defined DO_HAP (
    call :hap
    if errorlevel 1 exit /b 1
)

echo.
if defined DO_HAP (
    echo [build] done.
) else (
    rem The abi is named back only when it is NOT the default, so the hint on
    rem the common path is the command someone would actually type.
    if /I "%ABI%"=="arm64-v8a" ( set "HINT=" ) else ( set "HINT=%ABI% " )
    echo [build] done. Run `build.cmd !HINT!hap` to package the app, or open
    echo [build] harmony\ in DevEco Studio.
)
exit /b 0

rem ===========================================================================
:usage
echo Usage: build.cmd [abi] [deps^|llama^|mnn^|hap^|all] [clean] [arch=^<march^>^|baseline]
echo.
echo   abi     arm64-v8a ^(default^) ^| x86_64 ^| armeabi-v7a
echo   deps    the core's C++ dependencies via vcpkg   ^(required^)
echo   llama   the on-device LLM engine                ^(turns on-device chat on^)
echo   mnn     MNN with the omni Talker, for evaluation ^(NOT part of `all`^)
echo   hap     compile and sign the app, via hvigor    ^(no IDE needed^)
echo   all     all three, in that order
echo   clean   discard llama's cached build tree and SDK first
echo   arch=   llama only: -march feature set, or `baseline` for armv8-a
echo.
echo Naming no target means `all`, so a bare build.cmd builds everything. For
echo the app WITHOUT the on-device engine: build.cmd deps hap
echo.
echo Read the top of this file for the env overrides and the reasoning.
exit /b 0

rem ===========================================================================
rem The core's C++ dependencies, via vcpkg.
rem
rem vcpkg ships OHOS triplets but has taught no *port* about the platform, and
rem the pinned tool does not map CMAKE_SYSTEM_NAME=OHOS to its own toolchain.
rem Both are worked around by the overlay triplets in harmony\vcpkg-triplets\ --
rem read the comments there before changing anything here.
rem
rem Re-running is idempotent: vcpkg skips already-built ports and the destination
rem is refreshed.
rem ===========================================================================
:deps
rem READ from vcpkg.json's "builtin-baseline" rather than repeating the hash
rem here: two copies of a pin is one copy that silently goes stale.
rem tokens=4 delims=" splits `  "builtin-baseline": "<hash>",` into ["  "]
rem ["builtin-baseline"][": "][<hash>], so token 4 is the hash.
set "VCPKG_JSON=%~dp0..\vcpkg.json"
set "BASELINE="
for /f tokens^=4^ delims^=^" %%A in ('findstr /c:"builtin-baseline" "%VCPKG_JSON%"') do set "BASELINE=%%A"
if not defined BASELINE (
    echo no "builtin-baseline" in "%VCPKG_JSON%">&2
    exit /b 1
)

rem --- Space-free SDK path (mandatory for this half) ----------------------------
rem OpenSSL's generated Makefile hands the compiler path and --sysroot to /bin/sh
rem UNQUOTED, so a space anywhere in the SDK path breaks the build with
rem "/bin/sh: .../Huawei/DevEco: No such file or directory". A directory junction
rem is enough to work around it. Detect a space via substitution: !VAR: =! strips
rem spaces, so a differing result means VAR had one.
rem
rem Only :deps needs this -- llama.cpp is a plain CMake/Ninja build that quotes
rem its paths -- but the alias is left in OHOS_SDK_ROOT for the rest of the run,
rem which is harmless: it points at the same SDK.
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
        echo [deps] SDK path has a space; linking a space-free alias: !SDK_LINK!
        if exist "!SDK_LINK!" rmdir "!SDK_LINK!" 2>nul
        mklink /J "!SDK_LINK!" "!OHOS_SDK_ROOT!" >nul
        if errorlevel 1 (
            echo could not create the junction ^(mklink /J^)>&2
            exit /b 1
        )
    )
    set "OHOS_SDK_ROOT=!SDK_LINK!"
)

echo [deps] triplet=!TRIPLET!  vcpkg=!VCPKG_ROOT!  baseline=!BASELINE!

rem --- Bootstrap vcpkg at the pinned baseline -----------------------------------
if not exist "!VCPKG_ROOT!\.git" (
    echo [deps] cloning vcpkg ...
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
echo [deps] installing: !PORTS!
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
rem Copy debug\ too: vcpkg's *Targets.cmake reference both release (lib\) and
rem debug (debug\lib\) imported locations, and CMake verifies both exist at
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
echo [deps] done -^> !DEST!
exit /b 0

rem ===========================================================================
rem The on-device LLM engine: llama.cpp cross-compiled for this ABI.
rem
rem WHY THE -march FLAGS: a cross build with no -march silently lands on baseline
rem armv8-a. CMake's feature probes compile AND RUN a test binary, which is
rem impossible when cross-compiling, so HAVE_MATMUL_INT8 /
rem HAVE_FP16_VECTOR_ARITHMETIC / HAVE_SVE all "fail" and ggml quietly drops its
rem fast kernels -- leaving dotprod, i8mm and fp16 unused on a phone that has all
rem three. GGML_CPU_ARM_ARCH states the target's features instead of probing.
rem
rem CPU ONLY, deliberately -- there is no backend option. A Vulkan build was
rem measured on this class of device and lost on every axis (roughly 5x slower
rem load, 24x slower prefill, 3.5x slower decode) while adding tens of MB of
rem SPIR-V to the HAP. The structural reason is that offload copies every tensor
rem into device memory that shares its bandwidth with the CPU, so there is no win
rem to be had; it is not a tuning problem.
rem
rem Output: prebuilt\llama-sdk\<abi>\{include,lib}, which
rem entry\src\main\cpp\CMakeLists.txt finds BY ITSELF -- the same contract as the
rem vcpkg deps above, and deliberately under the same parent. Build it and the
rem on-device lane is on; delete it and the core compiles its stub and the app
rem still builds. Nothing to paste into build-profile.json5. Static archives: a
rem HAP has nowhere to put a companion .so.
rem ===========================================================================
:llama
rem The feature set of a current arm64 phone SoC. `+sve` is deliberately absent:
rem on 128-bit-vector parts the SVE kernels displace the i8mm SMMLA ones without
rem a width advantage, which measured SLOWER on prefill. Pass arch=<...> to try
rem something else, or `baseline` to compile plain armv8-a.
if not defined ARCH_SET set "ARCH=armv8.6-a+i8mm+bf16+dotprod+fp16"

rem --- llama.cpp source: cloned once by hand, reused ----------------------------
rem Not cloned for you, unlike vcpkg above: this build wants a checkout you can
rem pin and re-measure against, not one silently refreshed underneath it.
if not defined LLAMA_SRC for %%I in ("%~dp0..\..") do set "LLAMA_SRC=%%~fI\llama.cpp"
if not exist "!LLAMA_SRC!\include\llama.h" (
    echo no llama.cpp at "!LLAMA_SRC!".>&2
    echo clone it, or set LLAMA_SRC to an existing checkout:>&2
    echo   git clone --depth 1 https://github.com/ggml-org/llama.cpp "!LLAMA_SRC!">&2
    exit /b 1
)

if defined ARCH ( echo [llama] arch=!ARCH! ) else ( echo [llama] arch=baseline armv8-a )
echo [llama] src=!LLAMA_SRC!

rem The SDK ships its own cmake and ninja beside the toolchain; fall back to the
rem host's if it does not.
set "NATIVE=!OHOS_SDK_ROOT!\native"
set "TOOLCHAIN=!NATIVE!\build\cmake\ohos.toolchain.cmake"
set "CMAKE=!NATIVE!\build-tools\cmake\bin\cmake.exe"
set "NINJA=!NATIVE!\build-tools\cmake\bin\ninja.exe"
if not exist "!CMAKE!" for %%P in (cmake.exe) do set "CMAKE=%%~$PATH:P"
if not exist "!NINJA!" for %%P in (ninja.exe) do set "NINJA=%%~$PATH:P"
if not exist "!CMAKE!" (
    echo need cmake ^(the SDK's build-tools\cmake\bin, or on PATH^)>&2
    exit /b 1
)
if not exist "!NINJA!" (
    echo need ninja ^(the SDK's build-tools\cmake\bin, or on PATH^)>&2
    exit /b 1
)

set "LBUILD=%~dp0.llama-build\%ABI%"
rem In-tree under harmony\prebuilt\, for the same reason the vcpkg deps are: the
rem app's CMakeLists resolves it by repo-relative path, so there is no
rem machine-local absolute path to paste into a tracked file.
set "DEFAULT_LSDK=%~dp0prebuilt\llama-sdk\%ABI%"
if not defined LLAMA_SDK_DIR set "LLAMA_SDK_DIR=!DEFAULT_LSDK!"
set "LSDK=!LLAMA_SDK_DIR!"

if defined CLEAN (
    echo [llama] cleaning
    if exist "!LBUILD!" rmdir /s /q "!LBUILD!"
    if exist "!LSDK!"   rmdir /s /q "!LSDK!"
)

rem GGML_NATIVE=OFF because probing the host says nothing about the target.
rem LLAMA_CURL=OFF: the app has no model downloader, and curl here would drag in
rem a second, differently-configured copy of a dependency the core already links.
rem Static libraries only -- see the header above.
set "CFG=-S "!LLAMA_SRC!" -B "!LBUILD!" -G Ninja"
set "CFG=!CFG! -DCMAKE_MAKE_PROGRAM="!NINJA!""
set "CFG=!CFG! -DCMAKE_TOOLCHAIN_FILE="!TOOLCHAIN!""
set "CFG=!CFG! -DOHOS_ARCH=%ABI%"
set "CFG=!CFG! -DCMAKE_BUILD_TYPE=Release"
set "CFG=!CFG! -DBUILD_SHARED_LIBS=OFF"
set "CFG=!CFG! -DGGML_NATIVE=OFF"
set "CFG=!CFG! -DGGML_OPENMP=OFF"
set "CFG=!CFG! -DLLAMA_CURL=OFF"
set "CFG=!CFG! -DLLAMA_BUILD_TESTS=OFF"
set "CFG=!CFG! -DLLAMA_BUILD_EXAMPLES=OFF"
set "CFG=!CFG! -DLLAMA_BUILD_SERVER=OFF"
set "CFG=!CFG! -DLLAMA_BUILD_TOOLS=OFF"
if defined ARCH set "CFG=!CFG! -DGGML_CPU_ARM_ARCH=!ARCH!"

"!CMAKE!" !CFG!
if errorlevel 1 exit /b 1

rem Only the libraries. Upstream's CLI target links implementation libs that the
rem OFF switches above remove, so asking for it would fail the link -- and the
rem library is what we want.
"!CMAKE!" --build "!LBUILD!" --target llama ggml ggml-cpu ggml-base
if errorlevel 1 exit /b 1

echo [llama] assembling !LSDK!
if not exist "!LSDK!\include" mkdir "!LSDK!\include"
if not exist "!LSDK!\lib"     mkdir "!LSDK!\lib"

copy /y "!LLAMA_SRC!\include\llama.h" "!LSDK!\include\" >nul
if errorlevel 1 exit /b 1
copy /y "!LLAMA_SRC!\ggml\include\*.h" "!LSDK!\include\" >nul
if errorlevel 1 exit /b 1

rem Every archive is checked before it is copied, so a partial SDK stops the
rem script rather than being discovered later as a link error.
for %%L in (
    "!LBUILD!\src\libllama.a"
    "!LBUILD!\ggml\src\libggml.a"
    "!LBUILD!\ggml\src\libggml-cpu.a"
    "!LBUILD!\ggml\src\libggml-base.a"
) do (
    if not exist "%%~L" (
        echo expected archive missing: %%~L>&2
        exit /b 1
    )
    copy /y "%%~L" "!LSDK!\lib\" >nul
    if errorlevel 1 exit /b 1
)

echo [llama] done -^> !LSDK!
if /I not "!LSDK!"=="!DEFAULT_LSDK!" (
    echo [llama] that is NOT the auto-detected location, so name it in
    echo [llama] entry\build-profile.json5:
    echo [llama]   "arguments": "-DLLAMA_CPP_DIR=!LSDK!"
)
exit /b 0

rem ===========================================================================
rem The app itself: compile and sign the HAP, without opening the IDE.
rem
rem DevEco is an IntelliJ front end over hvigor, and hvigor is a command-line
rem tool -- so the IDE is a convenience, not a requirement. The three things it
rem otherwise sets up for you:
rem
rem   NODE_HOME         hvigor runs on node, and DevEco ships one.
rem   DEVECO_SDK_HOME   the SDK *root* (the dir holding default\), NOT the
rem                     openharmony dir OHOS_SDK_ROOT names. Getting this wrong
rem                     fails with "Invalid value of 'DEVECO_SDK_HOME' in the
rem                     system environment path", which does not hint that the
rem                     variable was simply unset.
rem   ohpm on PATH      for dependency resolution.
rem
rem Signing comes from the signingConfigs block in build-profile.json5, the same
rem one DevEco writes -- so a HAP built here is signed exactly as one built in
rem the IDE. Without that block the build still succeeds and produces only the
rem unsigned artifact, which is fine for CI and not installable on a device.
rem ===========================================================================
rem ===========================================================================
rem MNN with the omni Talker, cross-compiled for this ABI.
rem
rem This is an EVALUATION lane, not a dependency: nothing in the app links it,
rem and it is deliberately excluded from `all` so a bare build.cmd does not
rem spend twenty minutes on an engine the core does not use.
rem
rem It exists to answer one question that cannot be answered on a desktop.
rem MNN is the only way to get the model's own voice out -- llama.cpp has no
rem Talker checkpoint in GGUF -- and it works: measured, an 8.8 s clip in and
rem 4.0 s of real speech out. What is NOT known is whether generating that
rem speech is affordable on a phone. On a desktop CPU it runs at about 3x
rem slower than real time, which is the figure that decides the feature, and
rem a desktop figure does not transfer. See design/voice.md.
rem
rem Output: prebuilt\mnn-sdk\<abi>\{include,lib}, shaped like the llama SDK so
rem that whatever links it later resolves it by repo-relative path.
rem ===========================================================================
:mnn
if not defined MNN_SRC for %%I in ("%~dp0..\..") do set "MNN_SRC=%%~fI\MNN"
if not exist "!MNN_SRC!\CMakeLists.txt" (
    echo no MNN at "!MNN_SRC!".>&2
    echo clone it, or set MNN_SRC to an existing checkout:>&2
    echo   git clone --depth 1 https://github.com/alibaba/MNN "!MNN_SRC!">&2
    exit /b 1
)
echo [mnn] src=!MNN_SRC!

set "NATIVE=!OHOS_SDK_ROOT!\native"
set "TOOLCHAIN=!NATIVE!\build\cmake\ohos.toolchain.cmake"
set "CMAKE=!NATIVE!\build-tools\cmake\bin\cmake.exe"
set "NINJA=!NATIVE!\build-tools\cmake\bin\ninja.exe"
if not exist "!CMAKE!" for %%P in (cmake.exe) do set "CMAKE=%%~$PATH:P"
if not exist "!NINJA!" for %%P in (ninja.exe) do set "NINJA=%%~$PATH:P"
if not exist "!CMAKE!" (
    echo need cmake ^(the SDK's build-tools\cmake\bin, or on PATH^)>&2
    exit /b 1
)
if not exist "!NINJA!" (
    echo need ninja ^(the SDK's build-tools\cmake\bin, or on PATH^)>&2
    exit /b 1
)

set "MBUILD=%~dp0.mnn-build\%ABI%"
set "MSDK=%~dp0prebuilt\mnn-sdk\%ABI%"

if defined CLEAN (
    echo [mnn] cleaning
    if exist "!MBUILD!" rmdir /s /q "!MBUILD!"
    if exist "!MSDK!"   rmdir /s /q "!MSDK!"
)

rem MNN_BUILD_LLM_OMNI forces MNN_BUILD_OPENCV and MNN_BUILD_AUDIO on: the omni
rem path needs both front ends compiled in even when only audio is used at run
rem time. Which of them actually LOADS is a runtime flag -- is_visual / is_audio
rem / has_talker are plain booleans in the model's llm_config.json -- so the
rem 353 MB vision encoder costs nothing here beyond code size.
rem
rem MNN_ARM82 turns on the fp16 kernels every current arm64 phone has.
rem
rem MNN_SME2 has to be turned off EXPLICITLY -- it defaults ON, and leaving it
rem alone does not leave it off. Two reasons to disable it. It does not build
rem here: its kernels call the fp16 FML intrinsics (vfmlalq_low_f16), which need
rem `+fp16fml`, while MNN compiles those sources with `-march=armv8.2-a+fp16`,
rem and the OHOS clang does not supply the extension by default the way other
rem toolchains apparently do. And it would not pay: SME2 is Armv9, which no
rem phone this app targets has. Like the `+sve` note under :llama, it is a
rem feature to measure before enabling, not one to inherit from a default.
set "CFG=-S "!MNN_SRC!" -B "!MBUILD!" -G Ninja"
set "CFG=!CFG! -DCMAKE_MAKE_PROGRAM="!NINJA!""
set "CFG=!CFG! -DCMAKE_TOOLCHAIN_FILE="!TOOLCHAIN!""
set "CFG=!CFG! -DOHOS_ARCH=%ABI%"
set "CFG=!CFG! -DCMAKE_BUILD_TYPE=Release"
rem MNN has its OWN switch for this and ignores the standard CMake one: with
rem only BUILD_SHARED_LIBS=OFF it happily builds five .so files. MNN_SEP_BUILD
rem has to go with it -- it splits the backends into separate shared objects
rem and upstream documents it as meaningful only when shared.
set "CFG=!CFG! -DBUILD_SHARED_LIBS=OFF"
set "CFG=!CFG! -DMNN_BUILD_SHARED_LIBS=OFF"
set "CFG=!CFG! -DMNN_SEP_BUILD=OFF"
set "CFG=!CFG! -DMNN_BUILD_LLM=ON"
set "CFG=!CFG! -DMNN_BUILD_LLM_OMNI=ON"
set "CFG=!CFG! -DMNN_LOW_MEMORY=ON"
set "CFG=!CFG! -DMNN_SUPPORT_TRANSFORMER_FUSE=ON"
set "CFG=!CFG! -DMNN_BUILD_TOOLS=OFF"
set "CFG=!CFG! -DMNN_BUILD_BENCHMARK=OFF"
set "CFG=!CFG! -DMNN_BUILD_TEST=OFF"
set "CFG=!CFG! -DMNN_SME2=OFF"
if /I "%ABI%"=="arm64-v8a" set "CFG=!CFG! -DMNN_ARM82=ON"

"!CMAKE!" !CFG!
if errorlevel 1 exit /b 1

"!CMAKE!" --build "!MBUILD!" --target MNN llm
if errorlevel 1 exit /b 1

echo [mnn] assembling !MSDK!
if not exist "!MSDK!\include" mkdir "!MSDK!\include"
if not exist "!MSDK!\lib"     mkdir "!MSDK!\lib"

rem MNN's public headers, plus the llm engine's own. Copied rather than
rem referenced so the SDK is self-contained the way the llama one is.
xcopy /e /i /y /q "!MNN_SRC!\include\MNN" "!MSDK!\include\MNN" >nul
if errorlevel 1 exit /b 1
xcopy /e /i /y /q "!MNN_SRC!\transformers\llm\engine\include\llm" "!MSDK!\include\llm" >nul
if errorlevel 1 exit /b 1

rem And MNN's OWN rapidjson, which is not a convenience -- it is the fix for a
rem crash. rapidjson is header-only templates, so its member functions land in
rem the object files of whoever includes it, as weak symbols. The whole app is
rem one .so, the linker keeps exactly ONE definition of each, and this repo
rem already carries a much newer rapidjson under prebuilt\<abi>\include for
rem src/llm and src/platform. MNN's llm engine parses config.json with the 1.1.0
rem release it vendors.
rem
rem Two layouts, one symbol: the newer MemoryPoolAllocator keeps a SharedData*
rem where 1.1.0 keeps chunkHead_ and userBuffer_. Whichever copy wins the link
rem runs against the other one's objects. Measured: SIGSEGV at 0x10 inside
rem MemoryPoolAllocator::Malloc, on the FIRST json parse of Llm::createLLM,
rem before a single log line of MNN's own.
rem
rem So the SDK ships the version libMNN.a was compiled with, and the root
rem CMakeLists puts it ahead of the prebuilt one. One rapidjson per .so.
xcopy /e /i /y /q "!MNN_SRC!\3rd_party\rapidjson" "!MSDK!\include\rapidjson" >nul
if errorlevel 1 exit /b 1
if not exist "!MSDK!\include\rapidjson\document.h" (
    echo [mnn] no rapidjson under !MSDK!\include>&2
    echo [mnn] MNN vendors it at 3rd_party\rapidjson -- look there>&2
    exit /b 1
)

rem ONE archive, and that is correct: with MNN_SEP_BUILD=OFF the llm engine is
rem an OBJECT library, so its objects are folded into libMNN.a rather than
rem sitting beside it. There is no libllm.a to find, and looking for one reports
rem a successful build as a failure -- which is how this check first read it.
rem
rem Verified by symbol: Omni, Talker, generateWavform and setWavformCallback are
rem all inside the 14.5 MB archive.
rem
rem Checked before copying, so a partial SDK stops here rather than surfacing
rem later as a link error.
if not exist "!MBUILD!\libMNN.a" (
    echo [mnn] no libMNN.a under !MBUILD!>&2
    echo [mnn] MNN moves its output around between versions -- look there>&2
    exit /b 1
)
copy /y "!MBUILD!\libMNN.a" "!MSDK!\lib\" >nul
if errorlevel 1 exit /b 1

echo [mnn] SDK at !MSDK!
exit /b 0

rem ===========================================================================
rem qwentts.cpp: Qwen3-TTS, for the model's own voice reading text.
rem
rem A THIRD engine, and the bar for that is high -- design/voice.md has the
rem arithmetic. What it buys that neither llama.cpp nor MNN can: a voice that
rem holds across utterances AND keeps its prosody. llama.cpp runs the same
rem checkpoint, but its only reference path pools the speaker into a fixed
rem vector (attentive statistics pooling, [1536,T] -> [3072,1]) and the
rem time-varying part goes with it; MNN's Talker cannot be driven from text at
rem all. Measured on a desktop: VoiceDesign 1.16x real time, in-context cloning
rem 1.97x, against 3.0x for MNN's omni Talker.
rem
rem Narrow, unlike MNN: one purpose, ggml underneath, a C ABI in src/qwen.h
rem (qt_init / qt_synthesize / qt_free) and an upstream termux script, so it is
rem built for a phone by people other than us.
rem
rem STATIC, like the other two lanes -- qwen-core is a static library by default
rem and QWEN_SHARED stays off. A HAP has nowhere to put a companion .so.
rem
rem Output: prebuilt\qtts-sdk\<abi>\{include,lib}, shaped like the llama and MNN
rem SDKs so the app's CMakeLists finds it the same way.
rem ===========================================================================
:qtts
if not defined QTTS_SRC for %%I in ("%~dp0..\..") do set "QTTS_SRC=%%~fI\qwentts.cpp"
if not exist "!QTTS_SRC!\CMakeLists.txt" (
    echo no qwentts.cpp at "!QTTS_SRC!".>&2
    echo clone it, or set QTTS_SRC to an existing checkout:>&2
    echo   git clone --depth 1 --recursive https://github.com/ServeurpersoCom/qwentts.cpp "!QTTS_SRC!">&2
    exit /b 1
)
if not exist "!QTTS_SRC!\ggml\CMakeLists.txt" (
    echo qwentts.cpp at "!QTTS_SRC!" has no ggml submodule.>&2
    echo   git -C "!QTTS_SRC!" submodule update --init --recursive>&2
    exit /b 1
)
echo [qtts] src=!QTTS_SRC!

rem Two patches, applied to somebody else's checkout. patches\ carries the full
rem reasoning for each; in one line:
rem
rem   ggml-cpu.c   OHOS is the only platform taking the one __linux__ branch, and
rem                it calls functions the sysroot neither declares nor exports.
rem   backend.h    the CPU thread count is fixed at hardware_concurrency()/2 with
rem                no parameter, and that heuristic is about SMT, not phones.
rem
rem Applied here rather than committed there because the checkout is upstream's
rem and gets updated. :qpatch is idempotent -- see the note on it below.
call :qpatch "!QTTS_SRC!\ggml" qwentts-ggml-ohos-affinity.patch   || exit /b 1
call :qpatch "!QTTS_SRC!"      qwentts-thread-count-override.patch || exit /b 1

set "NATIVE=!OHOS_SDK_ROOT!\native"
set "TOOLCHAIN=!NATIVE!\build\cmake\ohos.toolchain.cmake"
set "CMAKE=!NATIVE!\build-tools\cmake\bin\cmake.exe"
set "NINJA=!NATIVE!\build-tools\cmake\bin\ninja.exe"
if not exist "!CMAKE!" for %%P in (cmake.exe) do set "CMAKE=%%~$PATH:P"
if not exist "!NINJA!" for %%P in (ninja.exe) do set "NINJA=%%~$PATH:P"
if not exist "!CMAKE!" (
    echo need cmake ^(the SDK's build-tools\cmake\bin, or on PATH^)>&2
    exit /b 1
)
if not exist "!NINJA!" (
    echo need ninja ^(the SDK's build-tools\cmake\bin, or on PATH^)>&2
    exit /b 1
)

set "QBUILD=%~dp0.qtts-build\%ABI%"
set "QSDK=%~dp0prebuilt\qtts-sdk\%ABI%"

if defined CLEAN (
    echo [qtts] cleaning
    if exist "!QBUILD!" rmdir /s /q "!QBUILD!"
    if exist "!QSDK!"   rmdir /s /q "!QSDK!"
)

rem The tools are the reason LLAMA-style flags are not enough here: qwen-tts,
rem qwen-codec and tts-server all build by default and all want a main(), which
rem a cross build has no use for. Only qwen-core is asked for by name below.
rem
rem GGML_NATIVE=OFF because -march=native is meaningless cross-compiling and the
rem probe runs on the host. The feature set comes from the same flags the llama
rem lane uses, for the same reason: CMake's feature tests compile AND RUN, which
rem cannot work for the phone, so every one of them "fails" and ggml silently
rem drops dotprod, i8mm and fp16 on a device that has all three.
set "QCFG=-S "!QTTS_SRC!" -B "!QBUILD!" -G Ninja"
set "QCFG=!QCFG! -DCMAKE_TOOLCHAIN_FILE="!TOOLCHAIN!""
set "QCFG=!QCFG! -DOHOS_ARCH=%ABI%"
set "QCFG=!QCFG! -DCMAKE_MAKE_PROGRAM="!NINJA!""
set "QCFG=!QCFG! -DCMAKE_BUILD_TYPE=Release"
set "QCFG=!QCFG! -DBUILD_SHARED_LIBS=OFF"
set "QCFG=!QCFG! -DQWEN_SHARED=OFF"
set "QCFG=!QCFG! -DGGML_NATIVE=OFF"
set "QCFG=!QCFG! -DGGML_OPENMP=OFF"
rem No -D_GNU_SOURCE here, though an earlier version of this file added it. The
rem first error was cpu_set_t and CPU_ZERO undeclared, which _GNU_SOURCE does
rem fix -- musl has them, and the OHOS toolchain's -D_XOPEN_SOURCE=600 hides
rem them. But the next error was pthread_setaffinity_np, which _GNU_SOURCE
rem cannot fix because musl does not have it at all. The flag was treating a
rem symptom of taking a branch that was never meant for this platform.
rem The patch above removes the branch, and with it the need for the flag.
rem
rem The same feature set the llama lane uses, and set the same way -- these two
rem are the same ggml compiled twice, so a difference here would be an accident
rem rather than a decision. `arch=` / `baseline` steer both.
if not defined ARCH_SET set "ARCH=armv8.6-a+i8mm+bf16+dotprod+fp16"
if /I "%ABI%"=="arm64-v8a" if defined ARCH set "QCFG=!QCFG! -DGGML_CPU_ARM_ARCH=!ARCH!"

rem The GPU, off unless asked for. It needs a Vulkan SDK on the HOST as well as
rem the driver on the phone, and four of the five flags below exist because the
rem two halves come from different places:
rem
rem   glslc, SPIRV-Headers   host only. 151 compute shaders are compiled at
rem                          build time by a vulkan-shaders-gen that ggml builds
rem                          for the host and then runs -- a host build nested
rem                          inside a cross build.
rem   Vulkan_INCLUDE_DIR     host, and this is the one that is NOT obvious. The
rem                          OHOS sysroot ships the C headers and ggml-vulkan
rem                          includes <vulkan/vulkan.hpp>, the C++ bindings,
rem                          which are not there. Compiling against the host
rem                          SDK's headers and linking the phone's loader is
rem                          fine: the loader ABI is stable and ggml queries
rem                          every extension at runtime anyway.
rem   Vulkan_LIBRARY         the phone's, from the sysroot.
rem
rem FIND_ROOT_PATH_MODE_PACKAGE=BOTH because the OHOS toolchain re-roots package
rem searches into the sysroot, so ggml's own `list(APPEND CMAKE_PREFIX_PATH
rem $ENV{VULKAN_SDK})` is ignored and SPIRV-Headers is never found.
rem
rem ninja on PATH because the nested host configure inherits the generator but
rem not CMAKE_MAKE_PROGRAM, and fails with "unable to find a build program
rem corresponding to Ninja" -- which reads like a missing tool rather than an
rem unpropagated variable.
if defined QTTS_VULKAN (
    if not defined VULKAN_SDK (
        echo [qtts] vulkan needs a Vulkan SDK on this machine ^(VULKAN_SDK is unset^).>&2
        exit /b 1
    )
    set "QVK=!VULKAN_SDK:\=/!"
    set "QSYSROOT=!NATIVE:\=/!/sysroot"
    set "QCFG=!QCFG! -DGGML_VULKAN=ON"
    set "QCFG=!QCFG! -DVulkan_GLSLC_EXECUTABLE=!QVK!/Bin/glslc.exe"
    set "QCFG=!QCFG! -DVulkan_INCLUDE_DIR=!QVK!/Include"
    set "QCFG=!QCFG! -DVulkan_LIBRARY=!QSYSROOT!/usr/lib/aarch64-linux-ohos/libvulkan.so"
    set "QCFG=!QCFG! -DSPIRV-Headers_DIR=!QVK!/Lib/cmake/SPIRV-Headers"
    set "QCFG=!QCFG! -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH"
    for %%D in ("!NINJA!") do set "PATH=%%~dpD;!PATH!"
    echo [qtts] vulkan backend ON, SDK !VULKAN_SDK!
)

echo [qtts] configuring
"!CMAKE!" !QCFG!
if errorlevel 1 exit /b 1

echo [qtts] building qwen-core
"!CMAKE!" --build "!QBUILD!" --target qwen-core
if errorlevel 1 exit /b 1

echo [qtts] assembling !QSDK!
if not exist "!QSDK!\include" mkdir "!QSDK!\include"
if not exist "!QSDK!\lib"     mkdir "!QSDK!\lib"

rem Only the C ABI header. Everything else under src\ is the implementation and
rem including any of it would tie this repo to qwentts.cpp's internals.
copy /y "!QTTS_SRC!\src\qwen.h" "!QSDK!\include\" >nul
if errorlevel 1 exit /b 1

rem Every .a the build produced: qwen-core plus ggml's, which are separate
rem archives (ggml-base, ggml-cpu, ggml). Collected by search rather than by
rem name because ggml splits them differently between versions, and a missing
rem one surfaces as an undefined symbol at the app's link step instead of here.
set "QLIBS=0"
for /R "%QBUILD%" %%F in (*.a) do (
    copy /y "%%~fF" "!QSDK!\lib\" >nul
    set /a QLIBS+=1
)
if !QLIBS! LSS 2 (
    echo [qtts] only !QLIBS! archive^(s^) under !QBUILD! -- expected qwen-core plus ggml's>&2
    exit /b 1
)
echo [qtts] SDK at !QSDK! ^(!QLIBS! archives^)
exit /b 0

rem Apply one patch from patches\ to a checkout, at most once.
rem
rem   %1  the git work tree to apply in (the repo, or one of its submodules)
rem   %2  the file name under harmony\patches\
rem
rem `--reverse --check` is the idempotence test, and it is a better one than a
rem marker file: it succeeds only when the change is ALREADY present, so a
rem re-run is a no-op, a checkout that moved under the patch fails loudly, and a
rem file somebody edited by hand is detected rather than patched twice.
:qpatch
set "QP_TREE=%~1"
set "QP_FILE=%~dp0patches\%~2"
if not exist "!QP_FILE!" (
    echo [qtts] no patch at "!QP_FILE!".>&2
    exit /b 1
)
git -C "!QP_TREE!" apply --reverse --check "!QP_FILE!" >nul 2>&1
if not errorlevel 1 (
    echo [qtts] already patched: %~2
    exit /b 0
)
echo [qtts] patching: %~2
git -C "!QP_TREE!" apply "!QP_FILE!"
if errorlevel 1 (
    echo [qtts] could not apply "!QP_FILE!" in "!QP_TREE!".>&2
    echo [qtts] the checkout moved under it; re-read the patch and rebase it.>&2
    exit /b 1
)
exit /b 0

:hap
if not defined DEVECO_HOME (
    echo DevEco Studio not found. Set DEVECO_HOME to its install directory.>&2
    exit /b 1
)
set "HVIGORW=!DEVECO_HOME!\tools\hvigor\bin\hvigorw.bat"
if not exist "!HVIGORW!" (
    echo no hvigorw at "!HVIGORW!" -- is DEVECO_HOME right?>&2
    exit /b 1
)

set "NODE_HOME=!DEVECO_HOME!\tools\node"
set "DEVECO_SDK_HOME=!DEVECO_HOME!\sdk"
set "PATH=!NODE_HOME!;!DEVECO_HOME!\tools\ohpm\bin;!PATH!"

echo [hap] hvigor from !DEVECO_HOME!
rem `call`, because hvigorw is itself a batch file and control has to come back.
call "!HVIGORW!" --mode module -p product=default assembleHap
if errorlevel 1 (
    echo [hap] build failed>&2
    exit /b 1
)

set "HAP_OUT=%~dp0entry\build\default\outputs\default"
echo [hap] done -^> !HAP_OUT!
if exist "!HAP_OUT!\entry-default-signed.hap" (
    echo [hap]   entry-default-signed.hap    -- installable: hdc install ^<file^>
)
if exist "!HAP_OUT!\entry-default-unsigned.hap" (
    echo [hap]   entry-default-unsigned.hap
)
exit /b 0
