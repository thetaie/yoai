#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Everything the HarmonyOS app needs built natively, before DevEco can compile
# the HAP. This does NOT build the app itself -- open harmony/ in DevEco for
# that. It builds what the app links against.
#
# The macOS/Linux counterpart of build.cmd: same tokens, defaults, flags and
# output layout. The reasoning behind the flags lives in build.cmd, in one
# place, so the two cannot drift into disagreeing about why.
#
#   ./build.sh [abi] [deps|llama|hap|all] [clean] [arch=<march>|baseline]
#
#     abi     arm64-v8a (default) | x86_64 | armeabi-v7a
#     deps    the core's C++ dependencies via vcpkg -> prebuilt/<abi>/
#             (REQUIRED: without it the native module fails to configure on its
#             first find_package)
#     llama   the on-device LLM engine  -> prebuilt/llama-sdk/<abi>/
#             (building it is what turns on-device chat on)
#     hap     compile and sign the app itself, via hvigor. No IDE needed.
#     all     all three, in that order -- and what a bare `./build.sh` does
#     clean   discard llama's cached build tree and SDK first. `deps` always
#             replaces its own output, so there is nothing for it to clean.
#     arch=   llama only: the -march feature set; `baseline` means armv8-a
#
# The abi defaults to arm64-v8a, which is the only one a HarmonyOS NEXT device
# has -- so it is normally left off entirely. Naming no target means `all`, so
# the first run is just:
#
#     ./build.sh
#
# and after that, just:
#
#     ./build.sh hap
#
# On-device chat is part of the app rather than an opt-in lane, so the bare
# command produces an app that has it. That is not free: it needs a llama.cpp
# checkout (~200 MB, and this script will not clone it for you -- see
# build_llama) plus a few minutes of cross-compile. To build the app WITHOUT the
# engine, name the two targets you do want:
#
#     ./build.sh deps hap
#
# Env overrides:
#   DEVECO_HOME     DevEco Studio / command-line-tools install (default: read
#                   from DevEco's own .home marker, else common locations)
#   VCPKG_ROOT      vcpkg checkout (default: a sibling of the repo)
#   OHOS_SDK_ROOT   the SDK dir CONTAINING native/ (default: from DEVECO_HOME)
#   LLAMA_SRC       llama.cpp checkout (default: a sibling of the repo)
#   LLAMA_SDK_DIR   where to assemble the llama SDK (default: auto-detected)
# ---------------------------------------------------------------------------
set -euo pipefail

# Resolved BEFORE the cd, because --help reads this header back out of the file
# and a relative BASH_SOURCE stops resolving once the working directory moves.
SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
cd "$(dirname "$SCRIPT")"
SCRIPT_DIR="$(pwd)"

# --- Arguments, position-independent -----------------------------------------
# Every token is a keyword or the ABI, and an unrecognized one is an ERROR
# rather than a guess. The predecessor treated any unknown token as llama's
# -march string, so a mistyped target silently became `-march=lama`.
ABI="" ; DO_DEPS="" ; DO_LLAMA="" ; DO_HAP="" ; ARCH="" ; ARCH_SET="" ; CLEAN=""
for tok in "$@"; do
    case "$tok" in
        deps)      DO_DEPS=1 ;;
        llama)     DO_LLAMA=1 ;;
        hap)       DO_HAP=1 ;;
        all)       DO_DEPS=1 ; DO_LLAMA=1 ; DO_HAP=1 ;;
        clean)     CLEAN=1 ;;
        baseline)  ARCH="" ; ARCH_SET=1 ;;
        arch=*)    ARCH="${tok#arch=}" ; ARCH_SET=1 ;;
        # The header block above, minus its rule lines, IS the help text -- so
        # there is only one copy of it to keep true. Delimited by the rules
        # rather than by line numbers: the hardcoded range this replaces had
        # already fallen one line behind the header it was printing.
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
# Nothing named: everything, in the order `all` would. The bare command is what
# a fresh clone runs, and it should end with an installable app that has
# on-device chat -- not with a half-built tree needing two more invocations
# nobody mentioned.
IMPLIED_ALL=""
if [ -z "$DO_DEPS" ] && [ -z "$DO_LLAMA" ] && [ -z "$DO_HAP" ]; then
    DO_DEPS=1 ; DO_LLAMA=1 ; DO_HAP=1 ; IMPLIED_ALL=1
fi

# OHOS ABI -> vcpkg overlay triplet (vcpkg-triplets/). Rejected here rather than
# at -DOHOS_ARCH, where an unknown ABI fails deep inside the toolchain file with
# nothing pointing back at the argument.
case "$ABI" in
  arm64-v8a)   TRIPLET="arm64-ohos" ;;
  x86_64)      TRIPLET="x64-ohos" ;;
  armeabi-v7a) TRIPLET="arm-ohos" ;;
  *) echo "[build] abi must be arm64-v8a|x86_64|armeabi-v7a (got '$ABI')" >&2; exit 2 ;;
esac
if [ -n "$DO_LLAMA" ] && [ "$ABI" = "armeabi-v7a" ]; then
    # Refusing outright is right when llama was ASKED for, and wrong when it was
    # merely implied by a bare command line -- on an ABI that cannot have the
    # engine, `./build.sh armeabi-v7a` should still build everything else.
    if [ -n "$IMPLIED_ALL" ]; then
        echo "[build] llama skipped: the on-device engine is built for arm64-v8a|x86_64 only"
        DO_LLAMA=""
    else
        echo "[build] the on-device engine is built for arm64-v8a|x86_64 only (got '$ABI')" >&2
        exit 2
    fi
fi

# --- Resolve DevEco Studio / the command-line tools --------------------------
# Everything below comes out of one install: the NDK for the two native halves,
# and node + hvigor for `hap`.
#
# The `.home` marker DevEco leaves in its per-version config directory holds the
# install path as one line, which beats guessing at install locations.
if [ -z "${DEVECO_HOME:-}" ]; then
    for marker in "$HOME/Library/Application Support/Huawei"/DevEcoStudio*/.home \
                  "$HOME/.config/Huawei"/DevEcoStudio*/.home
    do
        if [ -f "$marker" ]; then
            candidate="$(head -n 1 "$marker")"
            if [ -d "$candidate/tools" ]; then DEVECO_HOME="$candidate"; break; fi
        fi
    done
fi
if [ -z "${DEVECO_HOME:-}" ]; then
    for d in \
      "/Applications/DevEco-Studio.app/Contents" \
      "$HOME/.local/opt/ohos/command-line-tools" \
      "$HOME/command-line-tools"
    do
        if [ -d "$d/tools" ] || [ -d "$d/sdk" ]; then DEVECO_HOME="$d"; break; fi
    done
fi

# --- Resolve the OpenHarmony SDK ---------------------------------------------
# Both native halves need it, which is the main reason they now live in one
# script: this block used to exist in four copies and drift between them.
#
# OHOS_SDK_ROOT must name the directory CONTAINING native/ (vcpkg's
# scripts/toolchains/ohos.cmake appends /native itself).
if [ -z "${OHOS_SDK_ROOT:-}" ] && [ -n "${DEVECO_HOME:-}" ]; then
    OHOS_SDK_ROOT="$DEVECO_HOME/sdk/default/openharmony"
fi
if [ -z "${OHOS_SDK_ROOT:-}" ] || [ ! -f "$OHOS_SDK_ROOT/native/build/cmake/ohos.toolchain.cmake" ]; then
    echo "[build] OpenHarmony SDK not found. Set OHOS_SDK_ROOT to the directory containing native/," >&2
    echo "[build] or DEVECO_HOME to the DevEco Studio / command-line-tools install." >&2
    exit 1
fi

# Default the vcpkg checkout to a sibling of the repo so it is not committed.
VCPKG_ROOT="${VCPKG_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)/vcpkg}"

# --- Space-free SDK path -----------------------------------------------------
# OpenSSL's generated Makefile hands the compiler path and --sysroot to /bin/sh
# UNQUOTED, so a space anywhere in the SDK path breaks the build. A symlink is
# enough (build.cmd uses a junction for the same reason).
#
# Strictly this is only :deps's problem -- llama.cpp is a plain CMake/Ninja build
# that quotes its paths -- but it is done for the whole run because the alias
# points at the same SDK, so there is nothing to be gained by having two answers
# to "which SDK are we using" inside one script.
case "$OHOS_SDK_ROOT" in
  *\ *)
    case "$VCPKG_ROOT" in
      *\ *)
        echo "[build] SDK path contains a space ['$OHOS_SDK_ROOT'] and so does VCPKG_ROOT;" >&2
        echo "[build] point OHOS_SDK_ROOT at an SDK under a space-free path." >&2
        exit 1 ;;
    esac
    SDK_LINK="$VCPKG_ROOT/ohos-sdk"
    if [ ! -f "$SDK_LINK/native/build/cmake/ohos.toolchain.cmake" ]; then
      echo "[build] SDK path has a space; linking a space-free alias: $SDK_LINK"
      mkdir -p "$VCPKG_ROOT"
      rm -f "$SDK_LINK"
      ln -s "$OHOS_SDK_ROOT" "$SDK_LINK"
    fi
    OHOS_SDK_ROOT="$SDK_LINK"
    ;;
esac
# Exported because vcpkg's OHOS toolchain reads it from the environment of the
# port builds it spawns, not from anything we pass on the command line.
export OHOS_SDK_ROOT

echo "[build] abi=$ABI"
echo "[build] sdk=$OHOS_SDK_ROOT"
if [ -n "$DO_DEPS" ];  then echo "[build] target: deps  (vcpkg -> prebuilt/$ABI)"; fi
if [ -n "$DO_LLAMA" ]; then echo "[build] target: llama (llama.cpp -> prebuilt/llama-sdk/$ABI)"; fi
if [ -n "$DO_HAP" ];   then echo "[build] target: hap   (hvigor -> entry/build/default/outputs)"; fi
echo

#-----------------------------------------------------------------------------
# The core's C++ dependencies, via vcpkg.
#
# vcpkg ships OHOS triplets but has taught no *port* about the platform, and the
# pinned tool does not map CMAKE_SYSTEM_NAME=OHOS to its own toolchain. Both are
# worked around by the overlay triplets in vcpkg-triplets/ -- read the comments
# there before changing anything here.
#
# Re-running is idempotent: vcpkg skips already-built ports and the destination
# is refreshed.
#-----------------------------------------------------------------------------
build_deps() {
    # READ from vcpkg.json's "builtin-baseline" rather than repeating the hash
    # here: two copies of a pin is one copy that silently goes stale.
    local vcpkg_json="$SCRIPT_DIR/../vcpkg.json"
    local baseline
    baseline="$(sed -n 's/.*"builtin-baseline"[[:space:]]*:[[:space:]]*"\([0-9a-f]\{40\}\)".*/\1/p' "$vcpkg_json")"
    if [ -z "$baseline" ]; then
        echo "[deps] no 40-hex \"builtin-baseline\" in \"$vcpkg_json\"" >&2
        return 1
    fi

    echo "[deps] triplet=$TRIPLET  vcpkg=$VCPKG_ROOT  baseline=$baseline"

    # --- Bootstrap vcpkg at the pinned baseline ---
    if [ ! -d "$VCPKG_ROOT/.git" ]; then
        echo "[deps] cloning vcpkg ..."
        git clone https://github.com/microsoft/vcpkg "$VCPKG_ROOT"
    fi
    git -C "$VCPKG_ROOT" checkout "$baseline"
    if [ ! -x "$VCPKG_ROOT/vcpkg" ]; then
        "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
    fi

    # --classic: this script runs inside the repo, whose vcpkg.json would
    # otherwise put vcpkg into manifest mode (which rejects port-name arguments).
    #
    # zlib is named explicitly even though curl pulls it in: the OHOS triplet
    # builds everything static, so the final link needs it resolvable on its own.
    #
    # An array, and "curl[openssl]" quoted: unquoted, the brackets are a glob
    # the shell would try to expand against the working directory.
    local ports=(openssl "curl[openssl]" rapidjson zlib)
    echo "[deps] installing: ${ports[*]}"
    "$VCPKG_ROOT/vcpkg" install "${ports[@]}" --classic --triplet "$TRIPLET" \
        --overlay-triplets="$SCRIPT_DIR/vcpkg-triplets" \
        --x-buildtrees-root "$VCPKG_ROOT/buildtrees"

    local installed="$VCPKG_ROOT/installed/$TRIPLET"
    if [ ! -d "$installed" ]; then
        echo "[deps] expected install tree missing: $installed" >&2
        return 1
    fi

    local dest="$SCRIPT_DIR/prebuilt/$ABI"
    rm -rf "$dest"
    mkdir -p "$dest"
    # Copy debug/ too: vcpkg's *Targets.cmake reference both release (lib/) and
    # debug (debug/lib/) imported locations, and CMake verifies both exist at
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
# The on-device LLM engine: llama.cpp cross-compiled for this ABI.
#
# See build.cmd for the full reasoning: why the -march flags have to be stated
# (a cross build cannot RUN CMake's feature probes, so ggml silently drops its
# fast kernels and lands on baseline armv8-a), and why this is CPU only (a
# Vulkan build measured worse on every axis on this class of device).
#-----------------------------------------------------------------------------
build_llama() {
    # The feature set of a current arm64 phone SoC. `+sve` is deliberately
    # absent: on 128-bit-vector parts the SVE kernels displace the i8mm SMMLA
    # ones without a width advantage, which measured SLOWER on prefill.
    if [ -z "$ARCH_SET" ]; then
        ARCH="armv8.6-a+i8mm+bf16+dotprod+fp16"
    fi

    # Not cloned for you, unlike vcpkg above: this build wants a checkout you can
    # pin and re-measure against, not one silently refreshed underneath it.
    local llama_src="${LLAMA_SRC:-$(cd "$SCRIPT_DIR/../.." && pwd)/llama.cpp}"
    if [ ! -f "$llama_src/include/llama.h" ]; then
        echo "[llama] no llama.cpp at \"$llama_src\"." >&2
        echo "[llama] clone it, or set LLAMA_SRC to an existing checkout:" >&2
        echo "[llama]   git clone --depth 1 https://github.com/ggml-org/llama.cpp \"$llama_src\"" >&2
        return 1
    fi

    if [ -z "$ARCH" ]; then echo "[llama] arch=baseline armv8-a"; else echo "[llama] arch=$ARCH"; fi
    echo "[llama] src=$llama_src"

    # The SDK ships its own cmake and ninja beside the toolchain; fall back to
    # the host's if it does not.
    local native="$OHOS_SDK_ROOT/native"
    local toolchain="$native/build/cmake/ohos.toolchain.cmake"
    local cmake_bin="$native/build-tools/cmake/bin/cmake"
    local ninja_bin="$native/build-tools/cmake/bin/ninja"
    [ -x "$cmake_bin" ] || cmake_bin="$(command -v cmake || true)"
    [ -x "$ninja_bin" ] || ninja_bin="$(command -v ninja || true)"
    if [ -z "$cmake_bin" ] || [ -z "$ninja_bin" ]; then
        echo "[llama] need cmake and ninja (the SDK's build-tools/cmake/bin, or on PATH)." >&2
        return 1
    fi

    local lbuild="$SCRIPT_DIR/.llama-build/$ABI"
    # In-tree under prebuilt/, for the same reason the vcpkg deps are: the app's
    # CMakeLists resolves it by repo-relative path, so there is no machine-local
    # absolute path to paste into a tracked file.
    local default_lsdk="$SCRIPT_DIR/prebuilt/llama-sdk/$ABI"
    local lsdk="${LLAMA_SDK_DIR:-$default_lsdk}"

    if [ -n "$CLEAN" ]; then
        echo "[llama] cleaning"
        rm -rf "$lbuild" "$lsdk"
    fi

    # GGML_NATIVE=OFF because probing the host says nothing about the target.
    # LLAMA_CURL=OFF: the app has no model downloader, and curl here would drag
    # in a second, differently-configured copy of a dependency the core links.
    # Static libraries only -- a HAP has nowhere to put a companion .so.
    local cfg=(
      -S "$llama_src" -B "$lbuild" -G Ninja
      -DCMAKE_MAKE_PROGRAM="$ninja_bin"
      -DCMAKE_TOOLCHAIN_FILE="$toolchain"
      -DOHOS_ARCH="$ABI"
      -DCMAKE_BUILD_TYPE=Release
      -DBUILD_SHARED_LIBS=OFF
      -DGGML_NATIVE=OFF
      -DGGML_OPENMP=OFF
      -DLLAMA_CURL=OFF
      -DLLAMA_BUILD_TESTS=OFF
      -DLLAMA_BUILD_EXAMPLES=OFF
      -DLLAMA_BUILD_SERVER=OFF
      -DLLAMA_BUILD_TOOLS=OFF
    )
    if [ -n "$ARCH" ]; then
        cfg+=(-DGGML_CPU_ARM_ARCH="$ARCH")
    fi

    "$cmake_bin" "${cfg[@]}"

    # Only the libraries: upstream's CLI target links implementation libs the OFF
    # switches above remove, so asking for it would fail the link.
    "$cmake_bin" --build "$lbuild" --target llama ggml ggml-cpu ggml-base

    echo "[llama] assembling $lsdk"
    mkdir -p "$lsdk/include" "$lsdk/lib"
    cp "$llama_src/include/llama.h" "$lsdk/include/"
    cp "$llama_src"/ggml/include/*.h "$lsdk/include/"
    # Every archive is checked before it is copied, so a partial SDK stops the
    # script rather than being discovered later as a link error.
    local lib
    for lib in "$lbuild/src/libllama.a" \
               "$lbuild/ggml/src/libggml.a" \
               "$lbuild/ggml/src/libggml-cpu.a" \
               "$lbuild/ggml/src/libggml-base.a"
    do
        if [ ! -f "$lib" ]; then
            echo "[llama] expected archive missing: $lib" >&2
            return 1
        fi
        cp "$lib" "$lsdk/lib/"
    done

    echo "[llama] done -> $lsdk"
    if [ "$lsdk" != "$default_lsdk" ]; then
        echo "[llama] that is NOT the auto-detected location, so name it in"
        echo "[llama] entry/build-profile.json5:"
        echo "[llama]   \"arguments\": \"-DLLAMA_CPP_DIR=$lsdk\""
    fi
}

#-----------------------------------------------------------------------------
# The app itself: compile and sign the HAP, without opening the IDE.
#
# DevEco is an IntelliJ front end over hvigor, and hvigor is a command-line tool
# -- so the IDE is a convenience, not a requirement. The three things it
# otherwise sets up for you:
#
#   NODE_HOME         hvigor runs on node, and DevEco ships one.
#   DEVECO_SDK_HOME   the SDK *root* (the dir holding default/), NOT the
#                     openharmony dir OHOS_SDK_ROOT names. Getting this wrong
#                     fails with "Invalid value of 'DEVECO_SDK_HOME' in the
#                     system environment path", which does not hint that the
#                     variable was simply unset.
#   ohpm on PATH      for dependency resolution.
#
# Signing comes from the signingConfigs block in build-profile.json5, the same
# one DevEco writes -- so a HAP built here is signed exactly as one built in the
# IDE. Without that block the build still succeeds and produces only the
# unsigned artifact, which is fine for CI and not installable on a device.
#-----------------------------------------------------------------------------
build_hap() {
    if [ -z "${DEVECO_HOME:-}" ]; then
        echo "[hap] DevEco Studio not found. Set DEVECO_HOME to its install directory." >&2
        return 1
    fi

    local hvigorw="$DEVECO_HOME/tools/hvigor/bin/hvigorw"
    # The standalone command-line-tools package lays hvigorw out at bin/ instead.
    [ -x "$hvigorw" ] || hvigorw="$DEVECO_HOME/bin/hvigorw"
    if [ ! -x "$hvigorw" ]; then
        echo "[hap] no hvigorw under \"$DEVECO_HOME\" -- is DEVECO_HOME right?" >&2
        return 1
    fi

    export NODE_HOME="$DEVECO_HOME/tools/node"
    export DEVECO_SDK_HOME="$DEVECO_HOME/sdk"
    export PATH="$NODE_HOME/bin:$NODE_HOME:$DEVECO_HOME/tools/ohpm/bin:$PATH"

    echo "[hap] hvigor from $DEVECO_HOME"
    "$hvigorw" --mode module -p product=default assembleHap

    local out="$SCRIPT_DIR/entry/build/default/outputs/default"
    echo "[hap] done -> $out"
    if [ -f "$out/entry-default-signed.hap" ]; then
        echo "[hap]   entry-default-signed.hap    -- installable: hdc install <file>"
    fi
    if [ -f "$out/entry-default-unsigned.hap" ]; then
        echo "[hap]   entry-default-unsigned.hap"
    fi
}

if [ -n "$DO_DEPS" ];  then build_deps;  fi
if [ -n "$DO_LLAMA" ]; then build_llama; fi
if [ -n "$DO_HAP" ];   then build_hap;   fi

echo
if [ -n "$DO_HAP" ]; then
    echo "[build] done."
else
    # The abi is named back only when it is NOT the default, so the hint on the
    # common path is the command someone would actually type.
    hint=""
    [ "$ABI" = "arm64-v8a" ] || hint="$ABI "
    echo "[build] done. Run \`./build.sh ${hint}hap\` to package the app, or open"
    echo "[build] harmony/ in DevEco Studio."
fi
