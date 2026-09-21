#!/usr/bin/env bash
# Desktop client build: the Node-API addon, then the Electron bundles.
#
# The addon is the slow half and changes rarely; the bundles are seconds. They
# are separate arguments for that reason -- the loop while writing UI is
# `npm run dev`, and nothing here has to run again until the C++ changes.
#
# Usage: ./build.sh [clean] [addon] [llama] [app]
#
#   (no args)  addon + app
#   addon      just the Node-API addon (build/Release/bodyclock.node)
#   llama      assemble a desktop llama.cpp SDK and build the addon WITH the
#              on-device lane. Implies addon.
#   app        just the Electron bundles (out/)
#   clean      discard build/ first. Needed to change the lane set: CMake caches
#              it, so adding `llama` to a tree configured without it has no
#              effect.
#
# Env overrides:
#   VCPKG_ROOT        vcpkg checkout (default: a sibling of the repo, used only
#                     if it exists)
#   OPENSSL_ROOT_DIR  where to find OpenSSL, when the default search misses it
#   LLAMA_SRC         llama.cpp checkout (default: a sibling of the repo), `llama` only
set -euo pipefail

cd "$(dirname "$0")"
DESKTOP_DIR="$(pwd)"
PROJECT_DIR="$(cd .. && pwd)"
BUILD_DIR="$DESKTOP_DIR/build"

DO_CLEAN=""
DO_ADDON=""
DO_LLAMA=""
DO_APP=""
for arg in "$@"; do
  case "$arg" in
    clean) DO_CLEAN=1 ;;
    addon) DO_ADDON=1 ;;
    llama) DO_LLAMA=1; DO_ADDON=1 ;;
    app)   DO_APP=1 ;;
    *) echo "Unknown argument \"$arg\". Usage: ./build.sh [clean] [addon] [llama] [app]" >&2; exit 1 ;;
  esac
done

# No target named means both, which is what a first run wants.
if [ -z "$DO_ADDON" ] && [ -z "$DO_APP" ]; then
  DO_ADDON=1
  DO_APP=1
fi

if [ -n "$DO_CLEAN" ]; then
  rm -rf "$BUILD_DIR"
fi

if [ ! -d node_modules ]; then
  echo "[build] npm install"
  npm install
fi

#------------------------------------------------------------------------------
# The Node-API addon
#------------------------------------------------------------------------------
if [ -n "$DO_ADDON" ]; then
  CMAKE_JS_ARGS=()

  # Same policy as the root build: vcpkg is one way to get curl/openssl/
  # rapidjson, not a requirement -- the CMakeLists resolves all three with plain
  # find_package, which a distro's own packages satisfy. A VCPKG_ROOT the caller
  # set is a request, not a hint, so a wrong path is an error rather than a
  # silent fall back to something else.
  VCPKG_EXPLICIT="${VCPKG_ROOT:+yes}"
  VCPKG_ROOT="${VCPKG_ROOT:-$PROJECT_DIR/../vcpkg}"

  if [ -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]; then
    CMAKE_JS_ARGS+=(
      "--CDCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
      "--CDVCPKG_INSTALLED_DIR=$PROJECT_DIR/vcpkg_installed"
    )
  elif [ -n "$VCPKG_EXPLICIT" ]; then
    echo "vcpkg not found at $VCPKG_ROOT (from VCPKG_ROOT)." >&2
    exit 1
  else
    # macOS ships libcurl but no OpenSSL headers, and Homebrew leaves openssl@3
    # unlinked on purpose, so find_package(OpenSSL) comes up empty on a machine
    # that has it installed.
    if [ "$(uname -s)" = "Darwin" ] && [ -z "${OPENSSL_ROOT_DIR:-}" ]; then
      if BREW_OPENSSL="$(brew --prefix openssl@3 2>/dev/null)" && [ -d "$BREW_OPENSSL" ]; then
        CMAKE_JS_ARGS+=("--CDOPENSSL_ROOT_DIR=$BREW_OPENSSL")
      fi
    fi
  fi

  # A desktop llama.cpp SDK, shaped like the ones harmony/build.sh assembles per
  # ABI: include/ plus lib/. Unlike the root build's `asr`, this one needs
  # libllama as well as ggml, so it is the long build.
  if [ -n "$DO_LLAMA" ]; then
    LLAMA_SRC="${LLAMA_SRC:-$PROJECT_DIR/../llama.cpp}"
    if [ ! -f "$LLAMA_SRC/ggml/include/ggml.h" ]; then
      echo "no llama.cpp at $LLAMA_SRC." >&2
      echo "clone it at the pinned commit, with its patch applied:" >&2
      echo "  ../third/sync.sh clone llama.cpp" >&2
      echo "or point LLAMA_SRC at an existing checkout." >&2
      exit 1
    fi

    LBUILD="$BUILD_DIR/llama"
    LSDK="$BUILD_DIR/llama-sdk"
    echo "[build] llama.cpp from $LLAMA_SRC"
    if [ ! -f "$LBUILD/build.ninja" ] && [ ! -f "$LBUILD/Makefile" ]; then
      cmake -S "$LLAMA_SRC" -B "$LBUILD" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DLLAMA_BUILD_TESTS=OFF \
        -DLLAMA_BUILD_EXAMPLES=OFF \
        -DLLAMA_BUILD_TOOLS=OFF \
        -DLLAMA_BUILD_SERVER=OFF \
        -DLLAMA_CURL=OFF
    fi
    cmake --build "$LBUILD" --parallel --target llama ggml ggml-cpu ggml-base

    mkdir -p "$LSDK/include" "$LSDK/lib"
    cp "$LLAMA_SRC"/ggml/include/*.h "$LSDK/include/"
    cp "$LLAMA_SRC"/include/llama.h "$LSDK/include/"
    find "$LBUILD" \
      \( -name 'libllama.*' -o -name 'libggml.*' -o -name 'libggml-cpu.*' -o -name 'libggml-base.*' \) \
      -exec cp {} "$LSDK/lib/" \;
    echo "[build] llama SDK at $LSDK"

    CMAKE_JS_ARGS+=(--CDBODYCLOCK_ONDEVICE_LLM=ON "--CDLLAMA_CPP_DIR=$LSDK")
  fi

  # The addon is loaded by Electron, not by Node, so it is built against
  # Electron's headers. Read from the installed package rather than pinned here:
  # a version that disagrees with the one in node_modules is a module that
  # refuses to load, and the error names an ABI rather than a version.
  ELECTRON_VER="$(node -p "require('electron/package.json').version")"
  echo "[build] addon against electron $ELECTRON_VER"

  # `node <entry>` and not `npx cmake-js`: npx prepends node_modules/.bin to
  # PATH for everything it spawns, and one of cmake-js's own dependencies
  # installs a shim there called `rc`. That is harmless here and fatal on
  # Windows, where CMake picks it up as the resource compiler -- see build.cmd.
  # The same spelling on both platforms keeps the two scripts comparable.
  node "$DESKTOP_DIR/node_modules/cmake-js/bin/cmake-js" compile \
    --directory "$DESKTOP_DIR/native" \
    --out "$BUILD_DIR" \
    --runtime electron \
    --runtime-version "$ELECTRON_VER" \
    --CDCMAKE_BUILD_TYPE=Release \
    "${CMAKE_JS_ARGS[@]}"

  # A single-config generator writes it at the root of the output directory, a
  # multi-config one under Release/. Both are checked because the module loader
  # in src/main/core.ts searches both, and this message is the one place that
  # can say which one actually happened.
  ADDON="$BUILD_DIR/bodyclock.node"
  [ -f "$ADDON" ] || ADDON="$BUILD_DIR/Release/bodyclock.node"
  if [ ! -f "$ADDON" ]; then
    echo "[build] cmake-js reported success but there is no bodyclock.node under $BUILD_DIR" >&2
    exit 1
  fi
  echo "[build] addon at $ADDON"
fi

#------------------------------------------------------------------------------
# The Electron bundles
#------------------------------------------------------------------------------
if [ -n "$DO_APP" ]; then
  echo "[build] electron-vite"
  npm run build
fi
