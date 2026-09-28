#!/usr/bin/env bash
# The core library, the desktop tools in cli/ and the tests in tests/, into
# build/. Not the desktop app -- that is desktop/build.sh.
#
# The point of a desktop build is that the core can be exercised without a
# device -- see cli/README.md. It is not a shipping target.
#
# Dependencies come from vcpkg when it is there, and from the system's own
# packages when it is not -- see the comment above the vcpkg block below.
#
# With no arguments it configures if needed and builds, like every other build
# script in this repo. help, -h or --help prints usage().
#
# `tts` assembles a qwentts.cpp SDK into build/qtts-sdk/ and configures with
# -DYOAI_QTTS=ON -- speech output for the tools in cli/, without the
# desktop app.
#
# `asr` assembles a desktop ggml SDK into build/llama-sdk/ from a llama.cpp
# checkout and configures with -DYOAI_ASR=ON. Only ggml is built --
# recognition does not link llama -- so this is a couple of minutes, not the
# twenty the full engine takes.
#
# `llama` builds that full engine into the same SDK, libllama on top of the
# same ggml, and configures with -DYOAI_ONDEVICE_LLM=ON: chat and embedding on
# a GGUF, which cli/yoai-llama drives.
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: ./build.sh [clean] [asr] [llama] [tts]

Builds the core library, the desktop tools in cli/ and the tests in tests/,
into build/: configures if needed, then builds. This is not the desktop
app -- that is desktop/build.sh.

  asr      also compile speech recognition in. Needs a llama.cpp checkout
           for ggml; see LLAMA_SRC
  llama    also compile the on-device engine in: chat and embedding on a
           GGUF, for cli/yoai-llama. The same checkout, and a longer build
  tts     also compile speech output (Qwen3-TTS) in. Needs qwentts.cpp
           and llama.cpp checkouts; uses the GPU when there is one
  clean    delete build/ first
  help     this text; also -h or --help

They combine, e.g. ./build.sh clean asr tts

Environment, each also as YOAI_<NAME> in the environment or in .env,
which wins -- see .env.example:
  VCPKG_ROOT        vcpkg checkout (default: ../vcpkg, used only if it exists)
  OPENSSL_ROOT_DIR  where to find OpenSSL, when the default search misses it
  LLAMA_SRC         llama.cpp checkout for asr, llama and tts (default: ../llama.cpp)
  QTTS_SRC          qwentts.cpp checkout for tts (default: ../qwentts.cpp)
  VULKAN_SDK        Vulkan SDK, for tts on the GPU; optional
EOF
}

cd "$(dirname "$0")"
PROJECT_DIR="$(pwd)"
BUILD_DIR="$PROJECT_DIR/build"

# Defines yoai_load_env, called once the arguments are parsed.
. "$PROJECT_DIR/_load-env.sh"
THIRD_SYNC="$PROJECT_DIR/third/sync.sh"

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

DO_CLEAN=""
DO_ASR=""
DO_LLAMA=""
DO_TTS=""
for arg in "$@"; do
  case "$arg" in
    clean) DO_CLEAN=1 ;;
    asr)   DO_ASR=1 ;;
    llama) DO_LLAMA=1 ;;
    tts)   DO_TTS=1 ;;
    # Help exits 0: asking for it is not an error.
    help|-h|--help) usage; exit 0 ;;
    *) echo "Unknown argument \"$arg\". Run ./build.sh --help for the options." >&2; exit 1 ;;
  esac
done

# YOAI_* settings, from the environment and from .env. See .env.example.
# After the arguments, so the help text still prints when a configured path
# is wrong. The names are the paths this script reads: each one that is
# configured has to exist.
yoai_load_env VCPKG_ROOT OPENSSL_ROOT_DIR LLAMA_SRC QTTS_SRC VULKAN_SDK || exit 1

if [ -n "$DO_CLEAN" ]; then
  rm -rf "$BUILD_DIR"
fi

CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Release)

# vcpkg is one way to get curl and openssl, not a requirement: the CMakeLists
# resolves both with plain find_package, which a distro's own packages satisfy
# just as well (apt install libcurl4-openssl-dev libssl-dev). RapidJSON is
# vendored in third/. Cross-compiling for HarmonyOS is the case that really
# needs vcpkg, and that goes through harmony/build.sh instead.
#
# A VCPKG_ROOT the caller set is a request, not a hint: a wrong path is an
# error rather than a silent fall back to the system packages, which would
# build something other than what they asked for.
# `:+` rather than `+`: an empty VCPKG_ROOT means "no preference", not a
# request for a checkout at the empty path.
VCPKG_EXPLICIT="${VCPKG_ROOT:+yes}"
VCPKG_ROOT="${VCPKG_ROOT:-$(cd .. && pwd)/vcpkg}"

if [ -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]; then
  CMAKE_ARGS+=(
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
    -DVCPKG_INSTALLED_DIR="$PROJECT_DIR/vcpkg_installed"
  )
elif [ -n "$VCPKG_EXPLICIT" ]; then
  echo "vcpkg not found at $VCPKG_ROOT (from VCPKG_ROOT)." >&2
  exit 1
else
  # macOS ships libcurl but no OpenSSL headers, and Homebrew leaves openssl@3
  # unlinked on purpose (it would shadow the system LibreSSL), so
  # find_package(OpenSSL) comes up empty on a machine that has it installed.
  # Only in this branch: with vcpkg in play, OpenSSL comes from there.
  if [ "$(uname -s)" = "Darwin" ] && [ -z "${OPENSSL_ROOT_DIR:-}" ]; then
    if BREW_OPENSSL="$(brew --prefix openssl@3 2>/dev/null)" && [ -d "$BREW_OPENSSL" ]; then
      CMAKE_ARGS+=(-DOPENSSL_ROOT_DIR="$BREW_OPENSSL")
    fi
  fi
fi

# CMake caches which lanes are on, so adding `asr`, `llama` or `tts` to a tree
# that was configured without it would change nothing. Said here, before any
# SDK is built, rather than after minutes of building one.
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
  if [ -n "$DO_ASR" ] && ! grep -q "YOAI_ASR:BOOL=ON" "$BUILD_DIR/CMakeCache.txt"; then
    echo "[build] build/ was configured without asr; re-run as: ./build.sh clean asr" >&2
    exit 1
  fi
  if [ -n "$DO_LLAMA" ] && ! grep -q "YOAI_ONDEVICE_LLM:BOOL=ON" "$BUILD_DIR/CMakeCache.txt"; then
    echo "[build] build/ was configured without llama; re-run as: ./build.sh clean llama" >&2
    exit 1
  fi
  if [ -n "$DO_TTS" ] && ! grep -q "YOAI_QTTS:BOOL=ON" "$BUILD_DIR/CMakeCache.txt"; then
    echo "[build] build/ was configured without tts; re-run as: ./build.sh clean tts" >&2
    exit 1
  fi
fi

# A desktop ggml SDK, shaped like the one harmony/build.sh assembles per ABI:
# include/ plus lib/. Recognition links ggml and nothing else, so for `asr`
# alone only the three ggml targets are built. `llama` adds libllama and
# llama.h, built in the same tree on the same ggml.
if [ -n "$DO_ASR" ] || [ -n "$DO_LLAMA" ]; then
  LLAMA_SRC="${LLAMA_SRC:-$(cd .. && pwd)/llama.cpp}"
  if [ ! -f "$LLAMA_SRC/ggml/include/ggml.h" ]; then
    echo "no llama.cpp at $LLAMA_SRC." >&2
    echo "clone it at the pinned commit, with its patch applied:" >&2
    echo "  third/sync.sh clone llama.cpp" >&2
    echo "or point LLAMA_SRC at an existing checkout." >&2
    exit 1
  fi
  third_verify llama.cpp LLAMA_SRC "$LLAMA_SRC"

  GGML_BUILD="$BUILD_DIR/ggml"
  LSDK="$BUILD_DIR/llama-sdk"
  GGML_TARGETS=(ggml ggml-cpu ggml-base)
  if [ -n "$DO_LLAMA" ]; then
    GGML_TARGETS=(llama "${GGML_TARGETS[@]}")
  fi
  echo "[build] ${GGML_TARGETS[*]} from $LLAMA_SRC"
  if [ ! -f "$GGML_BUILD/build.ninja" ] && [ ! -f "$GGML_BUILD/Makefile" ]; then
    cmake -S "$LLAMA_SRC" -B "$GGML_BUILD" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DLLAMA_BUILD_TESTS=OFF \
      -DLLAMA_BUILD_EXAMPLES=OFF \
      -DLLAMA_BUILD_TOOLS=OFF \
      -DLLAMA_BUILD_SERVER=OFF \
      -DLLAMA_CURL=OFF
  fi
  cmake --build "$GGML_BUILD" --parallel --target "${GGML_TARGETS[@]}"

  mkdir -p "$LSDK/include" "$LSDK/lib"
  cp "$LLAMA_SRC"/ggml/include/*.h "$LSDK/include/"
  find "$GGML_BUILD" \( -name 'libggml.*' -o -name 'libggml-cpu.*' -o -name 'libggml-base.*' \
                        -o -name 'libllama.*' \) -exec cp {} "$LSDK/lib/" \;
  if [ -n "$DO_LLAMA" ]; then
    cp "$LLAMA_SRC/include/llama.h" "$LSDK/include/"
    if ! ls "$LSDK"/lib/libllama.* >/dev/null 2>&1; then
      echo "[build] llama built but libllama was not copied into $LSDK/lib" >&2
      exit 1
    fi
  fi
  echo "[build] llama.cpp SDK at $LSDK"

  CMAKE_ARGS+=(-DLLAMA_CPP_DIR="$LSDK")
  if [ -n "$DO_ASR" ]; then
    CMAKE_ARGS+=(-DYOAI_ASR=ON)
  fi
  if [ -n "$DO_LLAMA" ]; then
    CMAKE_ARGS+=(-DYOAI_ONDEVICE_LLM=ON)
  fi
fi

# A qwentts.cpp SDK for this build, so the core can speak without the desktop
# app. qwentts.cpp and llama.cpp are both required: qwentts compiles llama.cpp's
# ggml through GGML_SOURCE_DIR, so the core carries one ggml however many lanes
# are on. The GPU backend is not required -- see build.cmd's :qtts_sdk for the
# numbers -- and a build without one says so.
if [ -n "$DO_TTS" ]; then
  QTTS_SRC="${QTTS_SRC:-$(cd .. && pwd)/qwentts.cpp}"
  if [ ! -f "$QTTS_SRC/src/qwen.h" ]; then
    echo "no qwentts.cpp at $QTTS_SRC." >&2
    echo "clone it at the pinned commit, submodule and patch included:" >&2
    echo "  third/sync.sh clone qwentts.cpp" >&2
    echo "or point QTTS_SRC at an existing checkout." >&2
    exit 1
  fi
  third_verify qwentts.cpp QTTS_SRC "$QTTS_SRC"
  LLAMA_SRC="${LLAMA_SRC:-$(cd .. && pwd)/llama.cpp}"
  if [ ! -f "$LLAMA_SRC/ggml/CMakeLists.txt" ]; then
    echo "tts compiles llama.cpp's ggml, and there is none at $LLAMA_SRC/ggml." >&2
    echo "  third/sync.sh clone llama.cpp" >&2
    echo "or point LLAMA_SRC at an existing checkout." >&2
    exit 1
  fi
  third_verify llama.cpp LLAMA_SRC "$LLAMA_SRC"

  # The backend, per platform, as in desktop/build.sh: Metal on macOS, which
  # ggml turns on by itself for Apple; Vulkan elsewhere when there is one.
  QTTS_BACKEND=()
  QTTS_VULKAN=""
  if [ "$(uname -s)" = "Darwin" ]; then
    echo "[build] tts backend: Metal, which ggml enables for Apple by itself"
  elif [ -n "${VULKAN_SDK:-}" ] || command -v glslc >/dev/null 2>&1; then
    echo "[build] tts backend: Vulkan"
    QTTS_VULKAN=1
    QTTS_BACKEND=(-DGGML_VULKAN=ON)
    if [ -n "${VULKAN_SDK:-}" ]; then
      for d in "$VULKAN_SDK"/{lib,Lib}/cmake/SPIRV-Headers "$VULKAN_SDK"/{lib,Lib}/cmake; do
        if [ -f "$d/SPIRV-HeadersConfig.cmake" ]; then
          QTTS_BACKEND+=("-DSPIRV-Headers_DIR=$d")
          break
        fi
      done
    fi
  else
    echo "[build] warning: no Vulkan found, so tts is built for the CPU only." >&2
    echo "[build]   It works, slower: RTF 1.13 against 0.63 on the GPU where measured." >&2
    echo "[build]   For the GPU: apt install libvulkan-dev glslc (Debian/Ubuntu), or set" >&2
    echo "[build]   YOAI_VULKAN_SDK in .env; then ./build.sh clean tts." >&2
    QTTS_BACKEND=(-DGGML_VULKAN=OFF)
  fi

  QBUILD="$BUILD_DIR/qtts"
  QSDK="$BUILD_DIR/qtts-sdk"
  echo "[build] qwentts.cpp from $QTTS_SRC, ggml from $LLAMA_SRC/ggml"
  # Configured every run, with the backend passed each time, so it follows the
  # machine rather than whatever the first run cached.
  cmake -S "$QTTS_SRC" -B "$QBUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    "-DGGML_SOURCE_DIR=$LLAMA_SRC/ggml" \
    -DBUILD_SHARED_LIBS=OFF \
    -DQWEN_SHARED=OFF \
    ${QTTS_BACKEND[@]+"${QTTS_BACKEND[@]}"}
  # qwen-core only: qwen-tts, qwen-codec and tts-server all want a main().
  cmake --build "$QBUILD" --parallel --target qwen-core

  # Laid out from empty every run, and a Vulkan archive left in the tree by an
  # earlier build is taken out when this one is not using it: the core links
  # whatever the SDK directory holds.
  rm -rf "$QSDK"
  mkdir -p "$QSDK/include" "$QSDK/lib"
  cp "$QTTS_SRC"/src/qwen*.h "$QSDK/include/"
  find "$QBUILD" \( -name 'libqwen-core.*' -o -name 'libggml*.*' \) -exec cp {} "$QSDK/lib/" \;
  if [ -z "$QTTS_VULKAN" ]; then
    rm -f "$QSDK"/lib/libggml-vulkan.*
  fi
  if ! ls "$QSDK"/lib/libqwen-core.* >/dev/null 2>&1; then
    echo "[build] qwentts built, but no qwen-core library landed in $QSDK/lib" >&2
    exit 1
  fi
  echo "[build] tts SDK at $QSDK"

  CMAKE_ARGS+=(-DYOAI_QTTS=ON -DQTTS_DIR="$QSDK")
fi

if [ ! -f "$BUILD_DIR/build.ninja" ] && [ ! -f "$BUILD_DIR/Makefile" ]; then
  cmake -S . -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"
fi

cmake --build "$BUILD_DIR" --parallel
