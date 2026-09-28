#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Everything the Android app needs built natively, before Gradle can compile
# the APK. This does NOT build the app itself -- `./gradlew assembleDebug`, or
# open android/ in Android Studio, for that. It builds what the app links
# against.
#
# The Android counterpart of harmony/build.sh, and deliberately the same shape:
# same tokens, same prebuilt/<abi> layout, same rule that BUILDING AN SDK IS
# TURNING ITS LANE ON. What it is not is a port of that script's workarounds.
# The OHOS lane patches ggml because musl hides cpu_set_t behind _GNU_SOURCE and
# has no pthread_setaffinity_np at all; bionic needs neither, and ggml's
# affinity branch already names Android, so that patch does not apply here.
#
# THIS IS THE LINUX AND macOS SCRIPT. build.cmd is a separate, self-contained
# batch file doing the same job on Windows -- it used to invoke this one through
# bash, which broke as soon as PATH offered WSL's bash instead of Git's: WSL
# cannot open a D:\ path, and the failure reads as a missing script. Two native
# scripts are two things to keep in agreement; one that only works when the
# right bash happens to come first on PATH is worse.
#
# Nothing here may assume bash 4: macOS still ships 3.2 as /bin/bash, so no
# `mapfile`, no `declare -A`, no `${var,,}`.
#
#   ./build.sh [abi] [deps|llama|qtts|vulkan|apk|all] [clean] [arch=<march>|baseline]
#
#     abi     arm64-v8a (default) | x86_64
#     deps    the core's C++ dependencies via vcpkg -> prebuilt/<abi>/
#             (REQUIRED: without it the native module fails to configure on its
#             first find_package)
#     llama   the on-device LLM engine, and the ggml recognition rides on
#             -> prebuilt/llama-sdk/<abi>/
#     qtts    speech output (qwentts.cpp) -> prebuilt/qtts-sdk/<abi>/
#     vulkan  modifies qtts: also build ggml's Vulkan backend, for Adreno.
#             Implies qtts. Not a target of its own.
#     apk     assemble the debug APK via the Gradle wrapper
#     all     deps, llama, apk -- and what a bare `./build.sh` does
#     clean   discard the cached cross-build trees and SDKs first
#     arch=   llama/qtts only: the -march feature set; `baseline` means armv8-a
#
# **About arch=.** design/voice.md measures a cross build with no -march landing
# on baseline armv8-a and costing 2.9x prefill and 2.2x decode, because CMake's
# feature probes compile AND RUN a test binary and cannot run one for the phone.
# So the feature set is asserted here rather than detected. The default below is
# the HarmonyOS lane's, which is what a Kirin 9020 has; a current Qualcomm core
# is armv9.2-a and has more. The Device panel in the app prints what the chip
# actually reports, which is how the assertion gets checked -- set arch= to
# match it, and re-measure.
#
# Env overrides:
#   ANDROID_SDK_ROOT  the SDK. Default: ANDROID_HOME, then sdk.dir in
#                     android/local.properties
#   ANDROID_NDK_ROOT  the NDK (default: the version in android/gradle.properties)
#   VCPKG_ROOT        vcpkg checkout (default: a sibling of the repo)
#   LLAMA_SRC         llama.cpp checkout (default: a sibling of the repo)
#   QTTS_SRC          qwentts.cpp checkout (default: a sibling of the repo)
# ---------------------------------------------------------------------------
set -euo pipefail

SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
cd "$(dirname "$SCRIPT")"
SCRIPT_DIR="$(pwd)"
REPO_ROOT="$(cd .. && pwd)"

# --- Arguments, position-independent -----------------------------------------
# An unrecognized token is an ERROR rather than a guess, for the reason
# harmony/build.sh records: its predecessor treated any unknown token as the
# -march string, so a mistyped target silently became `-march=lama`.
ABI="" ; DO_DEPS="" ; DO_LLAMA="" ; DO_QTTS="" ; DO_APK="" ; QTTS_VULKAN=""
ARCH="" ; ARCH_SET="" ; CLEAN=""
for tok in "$@"; do
    case "$tok" in
        deps)      DO_DEPS=1 ;;
        llama)     DO_LLAMA=1 ;;
        qtts)      DO_QTTS=1 ;;
        vulkan)    DO_QTTS=1 ; QTTS_VULKAN=1 ;;
        apk)       DO_APK=1 ;;
        all)       DO_DEPS=1 ; DO_LLAMA=1 ; DO_APK=1 ;;
        clean)     CLEAN=1 ;;
        baseline)  ARCH="" ; ARCH_SET=1 ;;
        arch=*)    ARCH="${tok#arch=}" ; ARCH_SET=1 ;;
        -h|--help) sed '1,2d; /^# ---/,$d' "$SCRIPT" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)
            if [ -z "$ABI" ]; then
                ABI="$tok"
            else
                echo "[build] unknown argument '$tok'. Try: ./build.sh --help" >&2
                exit 2
            fi
            ;;
    esac
done

ABI="${ABI:-arm64-v8a}"
if [ -z "$DO_DEPS$DO_LLAMA$DO_QTTS$DO_APK" ]; then
    DO_DEPS=1 ; DO_LLAMA=1 ; DO_APK=1
fi

# The Kirin 9020's feature set, which is where every number in design/voice.md
# comes from. Kept as the default so a build on a new device is comparable
# against those before it is optimised for the new one.
if [ -z "$ARCH_SET" ]; then
    ARCH="armv8.6-a+i8mm+bf16+dotprod+fp16"
fi

case "$ABI" in
    arm64-v8a)   VCPKG_TRIPLET="arm64-android" ; ANDROID_ABI_CMAKE="arm64-v8a" ;;
    x86_64)      VCPKG_TRIPLET="x64-android"   ; ANDROID_ABI_CMAKE="x86_64" ; ARCH="" ;;
    *) echo "[build] unsupported abi '$ABI' (arm64-v8a, x86_64)" >&2 ; exit 2 ;;
esac

# The API level the NDK targets. 26 matches minSdk in app/build.gradle.kts; a
# mismatch is a link error naming a libc symbol rather than a configuration.
API_LEVEL=26

#-----------------------------------------------------------------------------
# Toolchain discovery
#-----------------------------------------------------------------------------

# Three sources, in the order of how explicitly each one was chosen.
# ANDROID_HOME is here because it is the variable the SDK's own installers and
# Android Studio still set -- looking only at ANDROID_SDK_ROOT means a machine
# with a perfectly good SDK reports not having one.
ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
if [ -z "$ANDROID_SDK_ROOT" ] && [ -f "$SCRIPT_DIR/local.properties" ]; then
    # local.properties is the file Gradle already reads, so there is one place
    # the SDK path lives rather than two that can disagree. Gradle escapes what
    # it writes there -- `C\:\\Users\\me\\...` -- so it is unescaped back into a
    # path before anything tries to open it.
    #
    # The [ -f ] guard is not politeness. Under `set -e` with pipefail, a sed
    # over a missing file fails the command substitution and kills the script
    # THERE, exit 2, in silence -- so the message below, which says exactly how
    # to fix it, could never print. That is how this lane looked broken.
    ANDROID_SDK_ROOT="$(sed -n 's/^sdk\.dir=//p' "$SCRIPT_DIR/local.properties" | head -1 \
        | sed 's/\\\\/\//g; s/\\:/:/g')"
fi
if [ -z "$ANDROID_SDK_ROOT" ] || [ ! -d "$ANDROID_SDK_ROOT" ]; then
    echo "[build] no Android SDK at '${ANDROID_SDK_ROOT:-<unset>}'." >&2
    echo "[build] set ANDROID_HOME or ANDROID_SDK_ROOT, or put sdk.dir in" >&2
    echo "[build]   $SCRIPT_DIR/local.properties" >&2
    exit 1
fi

if [ -z "${ANDROID_NDK_ROOT:-}" ]; then
    NDK_VER=""
    if [ -f "$SCRIPT_DIR/gradle.properties" ]; then
        NDK_VER="$(sed -n 's/^yoai\.ndk=//p' "$SCRIPT_DIR/gradle.properties" | head -1)"
    fi
    if [ -z "$NDK_VER" ]; then
        echo "[build] no yoai.ndk in $SCRIPT_DIR/gradle.properties" >&2
        exit 1
    fi
    ANDROID_NDK_ROOT="$ANDROID_SDK_ROOT/ndk/$NDK_VER"
fi
if [ ! -d "$ANDROID_NDK_ROOT" ]; then
    echo "[build] no NDK at $ANDROID_NDK_ROOT" >&2
    exit 1
fi

NDK_TOOLCHAIN="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake"
if [ ! -f "$NDK_TOOLCHAIN" ]; then
    echo "[build] no toolchain file at $NDK_TOOLCHAIN" >&2
    exit 1
fi

# The SDK's cmake and ninja, so the version is the one app/build.gradle.kts
# pins rather than whatever is on PATH.
CMAKE_BIN="$(ls -d "$ANDROID_SDK_ROOT"/cmake/*/bin/cmake* 2>/dev/null | head -1)"
CMAKE_BIN="${CMAKE_BIN:-$(command -v cmake || true)}"
NINJA_BIN="$(ls -d "$ANDROID_SDK_ROOT"/cmake/*/bin/ninja* 2>/dev/null | head -1)"
NINJA_BIN="${NINJA_BIN:-$(command -v ninja || true)}"
if [ -z "$CMAKE_BIN" ] || [ -z "$NINJA_BIN" ]; then
    echo "[build] need cmake and ninja (the SDK's cmake/*/bin, or on PATH)" >&2
    exit 1
fi

VCPKG_ROOT="${VCPKG_ROOT:-$(cd "$REPO_ROOT/.." && pwd)/vcpkg}"
LLAMA_SRC="${LLAMA_SRC:-$(cd "$REPO_ROOT/.." && pwd)/llama.cpp}"
QTTS_SRC="${QTTS_SRC:-$(cd "$REPO_ROOT/.." && pwd)/qwentts.cpp}"

echo "[build] abi=$ABI  api=$API_LEVEL  arch=${ARCH:-baseline}"
echo "[build] ndk=$ANDROID_NDK_ROOT"

if [ -n "$CLEAN" ]; then
    echo "[build] cleaning cross-build trees"
    rm -rf "$SCRIPT_DIR/.llama-build/$ABI" "$SCRIPT_DIR/.qtts-build/$ABI"
    rm -rf "$SCRIPT_DIR/prebuilt/llama-sdk/$ABI" "$SCRIPT_DIR/prebuilt/qtts-sdk/$ABI"
fi

# Shared by both engine builds. ANDROID_ABI/PLATFORM are what the NDK toolchain
# file reads; the rest is this project's policy.
#
# ANDROID_STL matches app/build.gradle.kts rather than taking the NDK's default
# of c++_static. For static archives the two are nearly the same thing -- an .a
# carries undefined references to libc++ either way, and it is the final shared
# library that decides which one resolves them -- so this is about not having to
# re-derive that every time the question comes up.
#
# A global array rather than a function printing lines into `mapfile`, because
# macOS still ships bash 3.2 as /bin/bash and `mapfile` arrived in bash 4. One
# builtin is not worth making every Mac install a second bash for.
NDK_CFG=(
    -G Ninja
    "-DCMAKE_MAKE_PROGRAM=$NINJA_BIN"
    "-DCMAKE_TOOLCHAIN_FILE=$NDK_TOOLCHAIN"
    "-DANDROID_ABI=$ANDROID_ABI_CMAKE"
    "-DANDROID_PLATFORM=android-$API_LEVEL"
    -DANDROID_STL=c++_shared
    -DCMAKE_BUILD_TYPE=Release
)

#-----------------------------------------------------------------------------
# deps -- the core's own dependencies, via vcpkg
#-----------------------------------------------------------------------------
build_deps() {
    # READ from vcpkg.json's "builtin-baseline" rather than repeating the hash
    # here: two copies of a pin is one copy that silently goes stale.
    local baseline
    baseline="$(sed -n 's/.*"builtin-baseline"[[:space:]]*:[[:space:]]*"\([0-9a-f]\{40\}\)".*/\1/p' \
        "$REPO_ROOT/vcpkg.json")"
    if [ -z "$baseline" ]; then
        echo "[deps] no 40-hex \"builtin-baseline\" in $REPO_ROOT/vcpkg.json" >&2
        return 1
    fi

    echo "[deps] triplet=$VCPKG_TRIPLET  vcpkg=$VCPKG_ROOT  baseline=$baseline"

    if [ ! -d "$VCPKG_ROOT/.git" ]; then
        echo "[deps] cloning vcpkg ..."
        git clone https://github.com/microsoft/vcpkg "$VCPKG_ROOT"
    fi
    git -C "$VCPKG_ROOT" checkout "$baseline"
    if [ ! -x "$VCPKG_ROOT/vcpkg" ]; then
        "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
    fi

    # No overlay triplet, unlike the OHOS lane: vcpkg ships arm64-android and
    # x64-android, and they are already static.
    export ANDROID_NDK_HOME="$ANDROID_NDK_ROOT"

    # Manifest mode, from the repository's vcpkg.json. The dependency list
    # lives there and nowhere else, so a port the core starts to need is added
    # once and every lane installs it. This used to be --classic with the
    # ports named here: a second list, which the desktop build never read.
    #
    # The install root is per triplet rather than VCPKG_ROOT/installed. A
    # manifest install may remove what its manifest does not ask for, and this
    # vcpkg checkout is shared by every ABI of both mobile lanes.
    local install_root="$VCPKG_ROOT/installed-$VCPKG_TRIPLET"
    echo "[deps] installing from vcpkg.json into $install_root"
    "$VCPKG_ROOT/vcpkg" install --triplet "$VCPKG_TRIPLET" \
        --x-manifest-root="$REPO_ROOT" \
        --x-install-root="$install_root" \
        --x-buildtrees-root "$VCPKG_ROOT/buildtrees" || return 1

    local installed="$install_root/$VCPKG_TRIPLET"
    if [ ! -d "$installed" ]; then
        echo "[deps] expected install tree missing: $installed" >&2
        return 1
    fi

    local dest="$SCRIPT_DIR/prebuilt/$ABI"
    rm -rf "$dest"
    mkdir -p "$dest"
    # debug/ too: vcpkg's *Targets.cmake reference both release (lib/) and debug
    # (debug/lib/) imported locations, and CMake verifies both exist at
    # find_package time.
    local sub
    for sub in include lib share debug; do
        if [ -d "$installed/$sub" ]; then
            cp -R "$installed/$sub" "$dest/$sub"
        fi
    done
    echo "[deps] done -> $dest"
}

#-----------------------------------------------------------------------------
# llama -- the on-device LLM, and the ggml that recognition links
#-----------------------------------------------------------------------------
build_llama() {
    if [ ! -d "$LLAMA_SRC" ]; then
        echo "[llama] no llama.cpp at $LLAMA_SRC." >&2
        echo "[llama]   ../third/sync.sh clone llama.cpp   # pinned commit, patch applied" >&2
        return 1
    fi

    local lbuild="$SCRIPT_DIR/.llama-build/$ABI"
    local lsdk="$SCRIPT_DIR/prebuilt/llama-sdk/$ABI"

    local cfg=("${NDK_CFG[@]}")
    cfg+=(
        -S "$LLAMA_SRC" -B "$lbuild"
        -DBUILD_SHARED_LIBS=OFF
        -DLLAMA_CURL=OFF
        -DLLAMA_BUILD_TESTS=OFF
        -DLLAMA_BUILD_EXAMPLES=OFF
        -DLLAMA_BUILD_SERVER=OFF
        -DLLAMA_BUILD_TOOLS=OFF
        # OFF, and this is not just a link fix.
        #
        # ggml turns OpenMP on wherever find_package(OpenMP) succeeds. It fails
        # on the OHOS sysroot, which has no libomp, so every figure in
        # design/voice.md was taken on **ggml's own thread pool**. The NDK does
        # ship libomp, so the same source would quietly run a different
        # scheduler here and a thread-count sweep would not be comparable with
        # the Kirin's -- which is most of what this lane exists to compare.
        #
        # It also fails to link if left on: the archives reference __kmpc_*
        # and nothing pulls the runtime into libyoai.so.
        -DGGML_OPENMP=OFF
    )
    if [ -n "$ARCH" ]; then
        cfg+=(-DGGML_CPU_ARM_ARCH="$ARCH")
    fi

    "$CMAKE_BIN" "${cfg[@]}"
    # Only the libraries: upstream's CLI target links implementation libs the
    # OFF switches above remove, so asking for it would fail the link.
    "$CMAKE_BIN" --build "$lbuild" --target llama ggml ggml-cpu ggml-base

    echo "[llama] assembling $lsdk"
    mkdir -p "$lsdk/include" "$lsdk/lib"
    cp "$LLAMA_SRC/include/llama.h" "$lsdk/include/"
    cp "$LLAMA_SRC"/ggml/include/*.h "$lsdk/include/"
    # Every archive is checked before it is copied, so a partial SDK stops the
    # script rather than being discovered later as a link error.
    local lib
    for lib in "$lbuild/src/libllama.a" \
               "$lbuild/ggml/src/libggml.a" \
               "$lbuild/ggml/src/libggml-cpu.a" \
               "$lbuild/ggml/src/libggml-base.a"
    do
        if [ ! -f "$lib" ]; then
            echo "[llama] missing $lib" >&2
            return 1
        fi
        cp "$lib" "$lsdk/lib/"
    done
    echo "[llama] done -> $lsdk"
}

#-----------------------------------------------------------------------------
# qtts -- speech output (qwentts.cpp), optionally with ggml's Vulkan backend
#-----------------------------------------------------------------------------
build_qtts() {
    if [ ! -d "$QTTS_SRC" ]; then
        echo "[qtts] no qwentts.cpp at $QTTS_SRC." >&2
        echo "[qtts]   ../third/sync.sh clone qwentts.cpp   # pinned commit, submodule, patches" >&2
        return 1
    fi
    if [ ! -d "$QTTS_SRC/ggml" ]; then
        echo "[qtts] $QTTS_SRC has no ggml submodule." >&2
        return 1
    fi

    # third/patches/ has two for this checkout. qwentts-ggml-ohos-affinity.patch
    # is for musl's missing pthread_setaffinity_np and does NOT apply here --
    # bionic has it. The thread-count one is not platform-specific: qwentts.cpp
    # picks hardware_concurrency()/2 reasoning about hyperthreads a phone does
    # not have. It is applied through the SAME patch file, so the two lanes
    # cannot drift into overriding the thread count differently.
    local patch="$REPO_ROOT/third/patches/qwentts-thread-count-override.patch"
    if [ -f "$patch" ]; then
        if git -C "$QTTS_SRC" apply --check --reverse "$patch" 2>/dev/null; then
            echo "[qtts] thread-count patch already applied"
        else
            echo "[qtts] applying $(basename "$patch")"
            git -C "$QTTS_SRC" apply "$patch"
        fi
    fi

    local qbuild="$SCRIPT_DIR/.qtts-build/$ABI"
    local qsdk="$SCRIPT_DIR/prebuilt/qtts-sdk/$ABI"

    # ONE ggml for both lanes, and llama.cpp's is the one. This used to compile
    # qwentts's own ggml submodule and the app linked that copy for everything,
    # because it superset llama's. The 2026-09-21 update inverted it: libllama
    # wants ggml_dsv4_hc_pre_gated, which the fork does not have, so whichever
    # copy is kept one symbol goes unmet and the app does not link.
    # GGML_SOURCE_DIR, added upstream the same day, points the qwentts build at
    # any ggml tree -- so both lanes compile the same one from the same commit.
    # The cost is the fork's fused snake kernel for the DAC decoder; see
    # design/voice.md. This target therefore needs a llama.cpp checkout even
    # when `llama` is not being built.
    if [ ! -f "$LLAMA_SRC/ggml/CMakeLists.txt" ]; then
        echo "[qtts] qtts compiles llama.cpp's ggml and there is none at $LLAMA_SRC/ggml." >&2
        echo "[qtts]   ../third/sync.sh clone llama.cpp" >&2
        return 1
    fi
    echo "[qtts] ggml=$LLAMA_SRC/ggml"

    local cfg=("${NDK_CFG[@]}")
    cfg+=(
        -S "$QTTS_SRC" -B "$qbuild"
        -DGGML_SOURCE_DIR="$LLAMA_SRC/ggml"
        -DBUILD_SHARED_LIBS=OFF
        # Same reason as the llama lane's: this ggml has to schedule threads the
        # way the one design/voice.md measured did.
        -DGGML_OPENMP=OFF
    )
    if [ -n "$ARCH" ]; then
        cfg+=(-DGGML_CPU_ARM_ARCH="$ARCH")
    fi
    if [ -n "$QTTS_VULKAN" ]; then
        # The reason this is worth a target at all: design/voice.md measures
        # ggml's Vulkan backend at RTF 0.60 on a laptop iGPU against 1.13 on its
        # CPU, and the HarmonyOS attempt died to a platform policy
        # (GPU_LEAK_KILL) rather than to anything about the backend. Android has
        # no such policy, so Adreno is the first real chance to find out.
        cfg+=(-DGGML_VULKAN=ON)
    fi

    "$CMAKE_BIN" "${cfg[@]}"
    "$CMAKE_BIN" --build "$qbuild"

    echo "[qtts] assembling $qsdk"
    mkdir -p "$qsdk/include" "$qsdk/lib"
    # qwen.h is what android/app/src/main/cpp/CMakeLists.txt probes for, so it
    # is the marker that says this SDK is complete.
    find "$QTTS_SRC" -maxdepth 3 -name 'qwen*.h' -exec cp {} "$qsdk/include/" \;
    # From LLAMA_SRC, not QTTS_SRC: the headers have to be the ones the archives
    # above were compiled from, and GGML_SOURCE_DIR moved that.
    cp "$LLAMA_SRC"/ggml/include/*.h "$qsdk/include/" 2>/dev/null || true
    find "$qbuild" -name 'libqwen-core.a' -exec cp {} "$qsdk/lib/" \;
    find "$qbuild" -name 'libggml*.a' -exec cp {} "$qsdk/lib/" \;

    if [ ! -f "$qsdk/include/qwen.h" ]; then
        echo "[qtts] no qwen.h landed in $qsdk/include -- the layout moved" >&2
        return 1
    fi
    echo "[qtts] done -> $qsdk"
    if [ -n "$QTTS_VULKAN" ]; then
        echo "[qtts] Vulkan backend included; the core links it when libggml-vulkan.a is present"
    fi
}

#-----------------------------------------------------------------------------
# apk
#-----------------------------------------------------------------------------
build_apk() {
    echo "[apk] assembleDebug"
    (cd "$SCRIPT_DIR" && ./gradlew assembleDebug -Pyoai.abi="$ABI")
    echo "[apk] done -> $SCRIPT_DIR/app/build/outputs/apk/debug/"
}

[ -n "$DO_DEPS" ]  && build_deps
[ -n "$DO_LLAMA" ] && build_llama
[ -n "$DO_QTTS" ]  && build_qtts
[ -n "$DO_APK" ]   && build_apk
exit 0
