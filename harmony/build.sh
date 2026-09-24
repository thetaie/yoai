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
#     mnn     MNN with the omni Talker -> prebuilt/mnn-sdk/<abi>/
#             An EVALUATION lane, deliberately NOT part of `all`: nothing in
#             the app links it. See build_mnn for what it is for.
#     qtts    speech output (qwentts.cpp) -> prebuilt/qtts-sdk/<abi>/
#             Needs a llama.cpp checkout too, whether or not `llama` is being
#             built: it compiles llama.cpp's ggml. See build_qtts for why.
#     vulkan  modifies qtts: also build ggml's Vulkan backend. Implies qtts,
#             and is not a target of its own.
#     hap     compile and sign the app itself, via hvigor. No IDE needed.
#     all     deps, llama, hap -- and what a bare `./build.sh` does
#     clean   discard the cached build trees and SDKs first. `deps` always
#             replaces its own output, so there is nothing for it to clean.
#     arch=   llama/qtts only: the -march feature set; `baseline` means armv8-a
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
#   Each also reads as YOAI_<NAME>, from the environment or from .env at the
#   repository root, and that wins over <NAME>. See .env.example.
#   DEVECO_HOME     DevEco Studio / command-line-tools install (default: read
#                   from DevEco's own .home marker, else common locations)
#   VCPKG_ROOT      vcpkg checkout (default: a sibling of the repo)
#   OHOS_SDK_ROOT   the SDK dir CONTAINING native/ (default: from DEVECO_HOME)
#   LLAMA_SRC       llama.cpp checkout (default: a sibling of the repo).
#                   Read by `llama` AND by `qtts`, which compiles its ggml.
#   LLAMA_SDK_DIR   where to assemble the llama SDK (default: auto-detected)
#   MNN_SRC         MNN checkout (default: a sibling of the repo), `mnn` only
#   QTTS_SRC        qwentts.cpp checkout (default: a sibling), `qtts` only
#   VULKAN_SDK      a host Vulkan SDK, required by `vulkan`
#
# `../third/sync.sh clone` fetches all of these at their pinned commits, with
# the patches this script expects already applied.
# ---------------------------------------------------------------------------
set -euo pipefail

# Resolved BEFORE the cd, because --help reads this header back out of the file
# and a relative BASH_SOURCE stops resolving once the working directory moves.
SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
cd "$(dirname "$SCRIPT")"
SCRIPT_DIR="$(pwd)"

# Defines yoai_load_env, called once the arguments are parsed.
. "$SCRIPT_DIR/../scripts/load-env.sh"
THIRD_SYNC="$SCRIPT_DIR/../third/sync.sh"

# Is this checkout what third/manifest.txt records? $1 the manifest name, $2
# its variable there, $3 the tree this build is about to compile.
#
# A WARNING, never a stop: a tree ahead of its pin is a normal state while
# trying something (third/README.md), and a missing patch matters to some
# lanes and not others. The tree goes over as YOAI_<var>, the one name sync's
# own loader will not override, so the tree checked is the tree built. Once
# per name per run.
THIRD_SEEN=","
third_verify() {
  case "$THIRD_SEEN" in *",$1,"*) return 0 ;; esac
  THIRD_SEEN="$THIRD_SEEN$1,"
  if ! env "YOAI_$2=$3" bash "$THIRD_SYNC" verify "$1"; then
    echo "[build] warning: $1 differs from third/manifest.txt (above) -- building anyway. Details: third/sync.sh check $1" >&2
  fi
}

# --- Arguments, position-independent -----------------------------------------
# Every token is a keyword or the ABI, and an unrecognized one is an ERROR
# rather than a guess. The predecessor treated any unknown token as llama's
# -march string, so a mistyped target silently became `-march=lama`.
ABI="" ; DO_DEPS="" ; DO_LLAMA="" ; DO_MNN="" ; DO_QTTS="" ; QTTS_VULKAN=""
DO_HAP="" ; ARCH="" ; ARCH_SET="" ; CLEAN=""
for tok in "$@"; do
    case "$tok" in
        deps)      DO_DEPS=1 ;;
        llama)     DO_LLAMA=1 ;;
        mnn)       DO_MNN=1 ;;
        qtts)      DO_QTTS=1 ;;
        # `vulkan` only modifies qtts -- it is not a target of its own, and it
        # implies one, so `./build.sh vulkan` does the useful thing rather than
        # nothing.
        vulkan)    DO_QTTS=1 ; QTTS_VULKAN=1 ;;
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

# YOAI_* settings, from the environment and from .env. See .env.example.
# After the arguments, so the help text still prints when a configured path
# is wrong. The names are the paths this script reads: each one that is
# configured has to exist.
yoai_load_env DEVECO_HOME VCPKG_ROOT OHOS_SDK_ROOT LLAMA_SRC MNN_SRC QTTS_SRC VULKAN_SDK || exit 1

ABI="${ABI:-arm64-v8a}"
# Nothing named: everything, in the order `all` would. The bare command is what
# a fresh clone runs, and it should end with an installable app that has
# on-device chat -- not with a half-built tree needing two more invocations
# nobody mentioned.
#
# NOTHING named means none of the five, not none of the three. The guard tested
# deps/llama/hap only, so `./build.sh qtts` -- a target it does not list -- read
# as an empty command line and quietly built deps, llama and the HAP as well.
IMPLIED_ALL=""
if [ -z "$DO_DEPS$DO_LLAMA$DO_MNN$DO_QTTS$DO_HAP" ]; then
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

    # Manifest mode, from the repository's vcpkg.json. The dependency list
    # lives there and nowhere else, so a port the core starts to need is added
    # once and every lane installs it. This used to be --classic with the
    # ports named here: a second list, which the desktop build never read.
    #
    # The install root is per triplet rather than VCPKG_ROOT/installed. A
    # manifest install may remove what its manifest does not ask for, and this
    # vcpkg checkout is shared by every ABI of both mobile lanes.
    local install_root="$VCPKG_ROOT/installed-$TRIPLET"
    echo "[deps] installing from vcpkg.json into $install_root"
    "$VCPKG_ROOT/vcpkg" install --triplet "$TRIPLET" \
        --x-manifest-root="$SCRIPT_DIR/.." \
        --x-install-root="$install_root" \
        --overlay-triplets="$SCRIPT_DIR/vcpkg-triplets" \
        --x-buildtrees-root "$VCPKG_ROOT/buildtrees" || return 1

    local installed="$install_root/$TRIPLET"
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
        echo "[llama] clone it at the pinned commit, with its patch applied:" >&2
        echo "[llama]   ../third/sync.sh clone llama.cpp" >&2
        echo "[llama] or set LLAMA_SRC to an existing checkout." >&2
        return 1
    fi

    if [ -z "$ARCH" ]; then echo "[llama] arch=baseline armv8-a"; else echo "[llama] arch=$ARCH"; fi
    echo "[llama] src=$llama_src"

    # The OHOS affinity patch, which this lane needs and never used to apply: it
    # was in the checkout by hand, undocumented, for long enough that nothing
    # here knew it was load-bearing. `--reverse --check` is the idempotence
    # test -- it succeeds only when the change is already present, so a re-run
    # is a no-op and a checkout that moved under the patch fails loudly.
    local patch="$SCRIPT_DIR/../third/patches/llama-ggml-ohos-affinity.patch"
    if [ -f "$patch" ]; then
        if git -C "$llama_src" apply --reverse --check "$patch" 2>/dev/null; then
            echo "[patch] already patched: $(basename "$patch")"
        else
            echo "[patch] patching: $(basename "$patch")"
            git -C "$llama_src" apply "$patch" || {
                echo "[patch] could not apply it; the checkout moved -- rebase the patch." >&2
                return 1; }
        fi
    fi
    third_verify llama.cpp LLAMA_SRC "$llama_src"

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

#-----------------------------------------------------------------------------
# mnn -- MNN with the omni Talker, cross-compiled for this ABI.
#
# An EVALUATION lane, not a dependency: nothing in the app links it, and it is
# deliberately excluded from `all` so a bare build does not spend twenty minutes
# on an engine the core does not use. It exists to answer one question a desktop
# cannot: MNN is the only way to get the model's own voice out -- llama.cpp has
# no Talker checkpoint in GGUF -- and what is not known is whether generating
# that speech is affordable on a phone. See design/voice.md.
#-----------------------------------------------------------------------------
build_mnn() {
    local mnn_src="${MNN_SRC:-$(cd "$SCRIPT_DIR/../.." && pwd)/MNN}"
    if [ ! -f "$mnn_src/CMakeLists.txt" ]; then
        echo "[mnn] no MNN at \"$mnn_src\"." >&2
        echo "[mnn] clone it at the pinned commit:" >&2
        echo "[mnn]   ../third/sync.sh clone MNN" >&2
        echo "[mnn] or set MNN_SRC to an existing checkout." >&2
        return 1
    fi
    echo "[mnn] src=$mnn_src"
    third_verify MNN MNN_SRC "$mnn_src"

    local native="$OHOS_SDK_ROOT/native"
    local toolchain="$native/build/cmake/ohos.toolchain.cmake"
    local cmake_bin="$native/build-tools/cmake/bin/cmake"
    local ninja_bin="$native/build-tools/cmake/bin/ninja"
    [ -x "$cmake_bin" ] || cmake_bin="$(command -v cmake || true)"
    [ -x "$ninja_bin" ] || ninja_bin="$(command -v ninja || true)"
    if [ -z "$cmake_bin" ] || [ -z "$ninja_bin" ]; then
        echo "[mnn] need cmake and ninja (the SDK's build-tools/cmake/bin, or on PATH)." >&2
        return 1
    fi

    local mbuild="$SCRIPT_DIR/.mnn-build/$ABI"
    local msdk="$SCRIPT_DIR/prebuilt/mnn-sdk/$ABI"
    if [ -n "$CLEAN" ]; then
        echo "[mnn] cleaning"
        rm -rf "$mbuild" "$msdk"
    fi

    # MNN_BUILD_LLM_OMNI forces MNN_BUILD_OPENCV and MNN_BUILD_AUDIO on: the omni
    # path needs both front ends compiled in even when only audio is used at run
    # time. Which of them actually LOADS is a runtime flag, so the 353 MB vision
    # encoder costs nothing here beyond code size.
    #
    # MNN has its OWN shared-library switch and ignores the standard CMake one:
    # with only BUILD_SHARED_LIBS=OFF it happily builds five .so files.
    # MNN_SEP_BUILD has to go with it -- it splits the backends into separate
    # shared objects and upstream documents it as meaningful only when shared.
    #
    # MNN_SME2 has to be turned off EXPLICITLY -- it defaults ON, and leaving it
    # alone does not leave it off. It does not build here (its kernels call the
    # fp16 FML intrinsics, which need +fp16fml, while MNN compiles those sources
    # with -march=armv8.2-a+fp16) and it would not pay: SME2 is Armv9, which no
    # phone this app targets has.
    local cfg=(
      -S "$mnn_src" -B "$mbuild" -G Ninja
      -DCMAKE_MAKE_PROGRAM="$ninja_bin"
      -DCMAKE_TOOLCHAIN_FILE="$toolchain"
      -DOHOS_ARCH="$ABI"
      -DCMAKE_BUILD_TYPE=Release
      -DBUILD_SHARED_LIBS=OFF
      -DMNN_BUILD_SHARED_LIBS=OFF
      -DMNN_SEP_BUILD=OFF
      -DMNN_BUILD_LLM=ON
      -DMNN_BUILD_LLM_OMNI=ON
      -DMNN_LOW_MEMORY=ON
      -DMNN_SUPPORT_TRANSFORMER_FUSE=ON
      -DMNN_BUILD_TOOLS=OFF
      -DMNN_BUILD_BENCHMARK=OFF
      -DMNN_BUILD_TEST=OFF
      -DMNN_SME2=OFF
    )
    # MNN_ARM82 turns on the fp16 kernels every current arm64 phone has.
    [ "$ABI" = "arm64-v8a" ] && cfg+=(-DMNN_ARM82=ON)

    "$cmake_bin" "${cfg[@]}"
    "$cmake_bin" --build "$mbuild" --target MNN llm

    echo "[mnn] assembling $msdk"
    mkdir -p "$msdk/include" "$msdk/lib"
    cp -R "$mnn_src/include/MNN" "$msdk/include/"
    cp -R "$mnn_src/transformers/llm/engine/include/llm" "$msdk/include/"

    # And MNN's OWN rapidjson, which is not a convenience -- it is the fix for a
    # crash. rapidjson is header-only templates, so its member functions land in
    # the object files of whoever includes it, as weak symbols. The whole app is
    # one .so, the linker keeps exactly ONE definition of each, and this repo
    # already carries a much newer rapidjson under prebuilt/<abi>/include.
    # Two layouts, one symbol: the newer MemoryPoolAllocator keeps a SharedData*
    # where 1.1.0 keeps chunkHead_ and userBuffer_. Measured: SIGSEGV at 0x10
    # inside MemoryPoolAllocator::Malloc, on the FIRST json parse.
    cp -R "$mnn_src/3rd_party/rapidjson" "$msdk/include/"
    if [ ! -f "$msdk/include/rapidjson/document.h" ]; then
        echo "[mnn] no rapidjson under $msdk/include" >&2
        echo "[mnn] MNN vendors it at 3rd_party/rapidjson -- look there" >&2
        return 1
    fi

    # ONE archive, and that is correct: with MNN_SEP_BUILD=OFF the llm engine is
    # an OBJECT library, so its objects are folded into libMNN.a rather than
    # sitting beside it. Looking for a libllm.a reports a successful build as a
    # failure -- which is how this check first read it.
    if [ ! -f "$mbuild/libMNN.a" ]; then
        echo "[mnn] no libMNN.a under $mbuild" >&2
        echo "[mnn] MNN moves its output around between versions -- look there" >&2
        return 1
    fi
    cp "$mbuild/libMNN.a" "$msdk/lib/"
    echo "[mnn] SDK at $msdk"
}

#-----------------------------------------------------------------------------
# qtts -- speech output (qwentts.cpp), optionally with ggml's Vulkan backend
#
# STATIC, like the other lanes -- a HAP has nowhere to put a companion .so.
# Output: prebuilt/qtts-sdk/<abi>/{include,lib}, shaped like the llama and MNN
# SDKs so the app's CMakeLists finds it the same way.
#-----------------------------------------------------------------------------
build_qtts() {
    local qtts_src="${QTTS_SRC:-$(cd "$SCRIPT_DIR/../.." && pwd)/qwentts.cpp}"
    if [ ! -f "$qtts_src/CMakeLists.txt" ]; then
        echo "[qtts] no qwentts.cpp at \"$qtts_src\"." >&2
        echo "[qtts]   ../third/sync.sh clone qwentts.cpp" >&2
        return 1
    fi
    echo "[qtts] src=$qtts_src"
    # The ggml submodule is deliberately not checked for: this lane stopped
    # compiling it, so demanding it would be a precondition on something the
    # build does not read.

    # ONE ggml for both lanes, and llama.cpp's is the one. This lane used to
    # compile qwentts's own ggml submodule and the app linked that copy for
    # everything, because it superset llama's. The 2026-09-21 update inverted
    # that: libllama now wants ggml_dsv4_hc_pre_gated, which the fork does not
    # have, so whichever of the two copies is kept, one symbol goes unmet and
    # the app does not link. GGML_SOURCE_DIR, which qwentts added the same day,
    # points its build at any ggml tree, so both lanes compile the SAME one from
    # the SAME commit. What it costs is the fork's fused snake kernel for the
    # DAC decoder; design/voice.md has the symbol tables.
    #
    # So this target needs a llama.cpp checkout even when `llama` is not built.
    local llama_src="${LLAMA_SRC:-$(cd "$SCRIPT_DIR/../.." && pwd)/llama.cpp}"
    if [ ! -f "$llama_src/ggml/CMakeLists.txt" ]; then
        echo "[qtts] qtts compiles llama.cpp's ggml and there is none at \"$llama_src/ggml\"." >&2
        echo "[qtts]   ../third/sync.sh clone llama.cpp" >&2
        return 1
    fi
    echo "[qtts] ggml=$llama_src/ggml"

    # Applied to llama_src, because that is the ggml being compiled now.
    # qwentts-ggml-ohos-affinity.patch is the same fix for the submodule, kept
    # in third/patches for whoever points GGML_SOURCE_DIR back at it, and
    # deliberately not applied here.
    local p
    for p in "llama-ggml-ohos-affinity.patch:$llama_src" \
             "qwentts-thread-count-override.patch:$qtts_src"
    do
        local file="$SCRIPT_DIR/../third/patches/${p%%:*}"
        local tree="${p#*:}"
        [ -f "$file" ] || { echo "[patch] no patch at $file" >&2; return 1; }
        if git -C "$tree" apply --reverse --check "$file" 2>/dev/null; then
            echo "[patch] already patched: ${p%%:*}"
        else
            echo "[patch] patching: ${p%%:*}"
            git -C "$tree" apply "$file" || {
                echo "[patch] could not apply it in $tree; rebase the patch." >&2
                return 1; }
        fi
    done
    third_verify llama.cpp LLAMA_SRC "$llama_src"
    third_verify qwentts.cpp QTTS_SRC "$qtts_src"

    local native="$OHOS_SDK_ROOT/native"
    local toolchain="$native/build/cmake/ohos.toolchain.cmake"
    local cmake_bin="$native/build-tools/cmake/bin/cmake"
    local ninja_bin="$native/build-tools/cmake/bin/ninja"
    [ -x "$cmake_bin" ] || cmake_bin="$(command -v cmake || true)"
    [ -x "$ninja_bin" ] || ninja_bin="$(command -v ninja || true)"
    if [ -z "$cmake_bin" ] || [ -z "$ninja_bin" ]; then
        echo "[qtts] need cmake and ninja (the SDK's build-tools/cmake/bin, or on PATH)." >&2
        return 1
    fi

    local qbuild="$SCRIPT_DIR/.qtts-build/$ABI"
    local qsdk="$SCRIPT_DIR/prebuilt/qtts-sdk/$ABI"
    if [ -n "$CLEAN" ]; then
        echo "[qtts] cleaning"
        rm -rf "$qbuild" "$qsdk"
    fi

    # The tools are why LLAMA-style flags are not enough: qwen-tts, qwen-codec
    # and tts-server all build by default and all want a main(), which a cross
    # build has no use for. Only qwen-core is asked for by name below.
    #
    # GGML_NATIVE=OFF because -march=native is meaningless cross-compiling and
    # the probe runs on the host. The feature set comes from the same flags the
    # llama lane uses, for the same reason: CMake's feature tests compile AND
    # RUN, which cannot work for the phone, so every one of them "fails" and
    # ggml silently drops dotprod, i8mm and fp16 on a device that has all three.
    local cfg=(
      -S "$qtts_src" -B "$qbuild" -G Ninja
      -DGGML_SOURCE_DIR="$llama_src/ggml"
      -DCMAKE_MAKE_PROGRAM="$ninja_bin"
      -DCMAKE_TOOLCHAIN_FILE="$toolchain"
      -DOHOS_ARCH="$ABI"
      -DCMAKE_BUILD_TYPE=Release
      -DBUILD_SHARED_LIBS=OFF
      -DQWEN_SHARED=OFF
      -DGGML_NATIVE=OFF
      -DGGML_OPENMP=OFF
    )
    # The same feature set the llama lane uses, and set the same way -- these
    # two are the same ggml compiled twice, so a difference here would be an
    # accident rather than a decision. `arch=` / `baseline` steer both.
    if [ -z "$ARCH_SET" ]; then
        ARCH="armv8.6-a+i8mm+bf16+dotprod+fp16"
    fi
    if [ "$ABI" = "arm64-v8a" ] && [ -n "$ARCH" ]; then
        cfg+=(-DGGML_CPU_ARM_ARCH="$ARCH")
    fi

    # The GPU, off unless asked for. It needs a Vulkan SDK on the HOST as well
    # as the driver on the phone, and the flags below exist because the two
    # halves come from different places:
    #
    #   glslc, SPIRV-Headers   host only. 151 compute shaders are compiled at
    #                          build time by a vulkan-shaders-gen that ggml
    #                          builds for the host and then runs -- a host build
    #                          nested inside a cross build.
    #   Vulkan_INCLUDE_DIR     host, and this is the one that is NOT obvious.
    #                          The OHOS sysroot ships the C headers and
    #                          ggml-vulkan includes <vulkan/vulkan.hpp>, the C++
    #                          bindings, which are not there. Compiling against
    #                          the host SDK's headers and linking the phone's
    #                          loader is fine: the loader ABI is stable and ggml
    #                          queries every extension at runtime anyway.
    #   Vulkan_LIBRARY         the phone's, from the sysroot.
    #
    # FIND_ROOT_PATH_MODE_PACKAGE=BOTH because the OHOS toolchain re-roots
    # package searches into the sysroot, so ggml's own CMAKE_PREFIX_PATH append
    # of VULKAN_SDK is ignored and SPIRV-Headers is never found.
    if [ -n "$QTTS_VULKAN" ]; then
        if [ -z "${VULKAN_SDK:-}" ]; then
            echo "[qtts] vulkan needs a Vulkan SDK on this machine (VULKAN_SDK is unset)." >&2
            return 1
        fi
        # The SDK lays its host tools out differently per platform: Bin/Include/
        # Lib on Windows, bin/include/lib everywhere else.
        local vkbin="$VULKAN_SDK/bin" vkinc="$VULKAN_SDK/include" vklib="$VULKAN_SDK/lib"
        [ -d "$vkbin" ] || vkbin="$VULKAN_SDK/Bin"
        [ -d "$vkinc" ] || vkinc="$VULKAN_SDK/Include"
        [ -d "$vklib" ] || vklib="$VULKAN_SDK/Lib"
        # And it lays SPIRV-Headers out two ways as well: the upstream layout
        # puts the config in a directory named for the package, which is the
        # only shape find_package looks for, and the LunarG SDK drops it
        # straight into cmake/ where nothing finds it. Naming the directory
        # covers both, since this flag is passed either way.
        local vkspirv="$vklib/cmake/SPIRV-Headers"
        [ -f "$vkspirv/SPIRV-HeadersConfig.cmake" ] || vkspirv="$vklib/cmake"
        cfg+=(
          -DGGML_VULKAN=ON
          -DVulkan_GLSLC_EXECUTABLE="$vkbin/glslc"
          -DVulkan_INCLUDE_DIR="$vkinc"
          -DVulkan_LIBRARY="$native/sysroot/usr/lib/aarch64-linux-ohos/libvulkan.so"
          -DSPIRV-Headers_DIR="$vkspirv"
          -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH
        )
        # ninja on PATH because the nested host configure inherits the generator
        # but not CMAKE_MAKE_PROGRAM, and fails with "unable to find a build
        # program corresponding to Ninja" -- which reads like a missing tool
        # rather than an unpropagated variable.
        PATH="$(dirname "$ninja_bin"):$PATH"
        export PATH
        echo "[qtts] vulkan backend ON, SDK $VULKAN_SDK"
    fi

    echo "[qtts] configuring"
    "$cmake_bin" "${cfg[@]}"
    echo "[qtts] building qwen-core"
    "$cmake_bin" --build "$qbuild" --target qwen-core

    echo "[qtts] assembling $qsdk"
    mkdir -p "$qsdk/include" "$qsdk/lib"
    # Only the C ABI header. Everything else under src/ is the implementation,
    # and including any of it would tie this repo to qwentts.cpp's internals.
    cp "$qtts_src/src/qwen.h" "$qsdk/include/"

    # Every .a the build produced: qwen-core plus ggml's, which are separate
    # archives. Collected by search rather than by name because ggml splits them
    # differently between versions, and a missing one surfaces as an undefined
    # symbol at the app's link step instead of here.
    local qlibs=0 lib
    while IFS= read -r lib; do
        cp "$lib" "$qsdk/lib/"
        qlibs=$((qlibs + 1))
    done < <(find "$qbuild" -name '*.a')
    if [ "$qlibs" -lt 2 ]; then
        echo "[qtts] only $qlibs archive(s) under $qbuild -- expected qwen-core plus ggml's" >&2
        return 1
    fi
    echo "[qtts] SDK at $qsdk ($qlibs archives)"
}

if [ -n "$DO_DEPS" ];  then build_deps;  fi
if [ -n "$DO_LLAMA" ]; then build_llama; fi
if [ -n "$DO_MNN" ];   then build_mnn;   fi
if [ -n "$DO_QTTS" ];  then build_qtts;  fi
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
