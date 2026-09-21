#!/usr/bin/env bash
# Desktop build: bodyclock_core plus the CLI harness, into build/.
#
# The point of a desktop build is that the core can be exercised without a
# device -- see cli/main.cpp. It is not a shipping target.
#
# Dependencies come from vcpkg when it is there, and from the system's own
# packages when it is not -- see the comment above the vcpkg block below.
#
# Usage: ./build.sh [clean] [asr]
#
#   asr   also build speech recognition, which needs ggml. Assembles a desktop
#         ggml SDK into build/llama-sdk/ from a llama.cpp checkout and configures
#         with -DBODYCLOCK_ASR=ON. Only ggml is built -- recognition does not
#         link llama -- so this is a couple of minutes, not the twenty the full
#         engine takes.
#
# Env overrides:
#   VCPKG_ROOT        vcpkg checkout (default: a sibling of the repo, used only
#                     if it exists)
#   OPENSSL_ROOT_DIR  where to find OpenSSL, when the default search misses it
#   LLAMA_SRC         llama.cpp checkout (default: a sibling of the repo), `asr` only
set -euo pipefail

cd "$(dirname "$0")"
PROJECT_DIR="$(pwd)"
BUILD_DIR="$PROJECT_DIR/build"

DO_CLEAN=""
DO_ASR=""
for arg in "$@"; do
  case "$arg" in
    clean) DO_CLEAN=1 ;;
    asr)   DO_ASR=1 ;;
    *) echo "Unknown argument \"$arg\". Usage: ./build.sh [clean] [asr]" >&2; exit 1 ;;
  esac
done

if [ -n "$DO_CLEAN" ]; then
  rm -rf "$BUILD_DIR"
fi

CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Release)

# vcpkg is one way to get curl/openssl/rapidjson, not a requirement: the
# CMakeLists resolves all three with plain find_package, which a distro's own
# packages satisfy just as well (apt install libcurl4-openssl-dev libssl-dev
# rapidjson-dev). Cross-compiling for HarmonyOS is the case that really needs
# vcpkg, and that goes through harmony/build.sh instead.
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

# A desktop ggml SDK, shaped like the one harmony/build.sh assembles per ABI:
# include/ plus lib/. Recognition links ggml and nothing else, so only the three
# ggml targets are built -- libllama is not needed and is not asked for.
if [ -n "$DO_ASR" ]; then
  LLAMA_SRC="${LLAMA_SRC:-$(cd .. && pwd)/llama.cpp}"
  if [ ! -f "$LLAMA_SRC/ggml/include/ggml.h" ]; then
    echo "no llama.cpp at $LLAMA_SRC." >&2
    echo "clone it at the pinned commit, with its patch applied:" >&2
    echo "  third/sync.sh clone llama.cpp" >&2
    echo "or point LLAMA_SRC at an existing checkout." >&2
    exit 1
  fi

  GGML_BUILD="$BUILD_DIR/ggml"
  LSDK="$BUILD_DIR/llama-sdk"
  echo "[build] ggml from $LLAMA_SRC"
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
  cmake --build "$GGML_BUILD" --parallel --target ggml ggml-cpu ggml-base

  mkdir -p "$LSDK/include" "$LSDK/lib"
  cp "$LLAMA_SRC"/ggml/include/*.h "$LSDK/include/"
  find "$GGML_BUILD" \( -name 'libggml.*' -o -name 'libggml-cpu.*' -o -name 'libggml-base.*' \) \
    -exec cp {} "$LSDK/lib/" \;
  echo "[build] ggml SDK at $LSDK"

  CMAKE_ARGS+=(-DBODYCLOCK_ASR=ON -DLLAMA_CPP_DIR="$LSDK")
fi

# CMake caches everything above, so adding `asr` to a build tree that was
# configured without it has no effect. Re-run with `clean` to change the set.
if [ ! -f "$BUILD_DIR/build.ninja" ] && [ ! -f "$BUILD_DIR/Makefile" ]; then
  cmake -S . -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"
elif [ -n "$DO_ASR" ] && ! grep -q "BODYCLOCK_ASR:BOOL=ON" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
  echo "[build] build/ was configured without asr; re-run as: ./build.sh clean asr" >&2
  exit 1
fi

cmake --build "$BUILD_DIR" --parallel
