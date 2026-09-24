#!/usr/bin/env bash
# Desktop client build: the Node-API addon, then the Electron bundles.
#
# The addon is the slow half and changes rarely; the bundles are seconds. They
# are separate arguments for that reason -- the loop while writing UI is
# `npm run dev`, and nothing here has to run again until the C++ changes.
#
# The Linux and macOS script; build.cmd is the Windows one. Keep the two in
# agreement -- and nothing here may assume bash 4, because macOS still ships 3.2
# as /bin/bash.
#
# Usage: ./build.sh [clean] [addon] [llama] [qtts] [app]
#
#   (no args)  addon + app
#   addon      just the Node-API addon (build/Release/bodyclock.node)
#   llama      assemble a desktop llama.cpp SDK and build the addon WITH the
#              on-device lane. Implies addon.
#   qtts       assemble a desktop qwentts.cpp SDK with the platform's GPU
#              backend and build the addon with speech output. Implies addon.
#              Needs a llama.cpp checkout too, whether or not `llama` is asked
#              for: it compiles llama.cpp's ggml. See the lane below for why.
#   app        just the Electron bundles (out/)
#   clean      discard build/ first. ADDING a lane no longer needs it -- the
#              addon is reconfigured every run, so `llama` on an existing tree
#              takes effect by itself. DROPPING one does: a lane that is not
#              asked for is not passed as OFF, it is not passed at all, and
#              CMake keeps the cached value. So `./build.sh addon` on a tree
#              built with `llama qtts` rebuilds it with both still on. The SDKs
#              under sdk/ are NOT discarded either way.
#
# Env overrides:
#   Each also reads as YOAI_<NAME>, from the environment or from .env at the
#   repository root, and that wins over <NAME>. See .env.example.
#   VCPKG_ROOT        vcpkg checkout (default: a sibling of the repo, used only
#                     if it exists)
#   OPENSSL_ROOT_DIR  where to find OpenSSL, when the default search misses it
#   LLAMA_SRC         llama.cpp checkout (default: a sibling of the repo).
#                     Read by `llama` AND by `qtts`, which compiles its ggml.
#   QTTS_SRC          qwentts.cpp checkout (default: a sibling), `qtts` only
set -euo pipefail

cd "$(dirname "$0")"
DESKTOP_DIR="$(pwd)"
PROJECT_DIR="$(cd .. && pwd)"

# Defines yoai_load_env, called once the arguments are parsed.
. "$PROJECT_DIR/scripts/load-env.sh"
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
BUILD_DIR="$DESKTOP_DIR/build"

# The SDKs live OUTSIDE build/, and that is not tidiness. build/ belongs to
# cmake-js: when its build step fails it runs its own CLEAN, which deletes the
# whole output directory. With the SDKs inside, one compile error in binding.cpp
# costs a full llama.cpp rebuild and a full Vulkan shader rebuild -- minutes,
# for a typo. `clean` still only discards build/, so the SDKs also survive a
# lane change, which is the other thing that used to rebuild them for nothing.
SDK_DIR="$DESKTOP_DIR/sdk"

DO_CLEAN=""
DO_ADDON=""
DO_LLAMA=""
DO_QTTS=""
DO_APP=""
for arg in "$@"; do
  case "$arg" in
    clean) DO_CLEAN=1 ;;
    addon) DO_ADDON=1 ;;
    llama) DO_LLAMA=1; DO_ADDON=1 ;;
    qtts)  DO_QTTS=1;  DO_ADDON=1 ;;
    app)   DO_APP=1 ;;
    *) echo "Unknown argument \"$arg\". Usage: ./build.sh [clean] [addon] [llama] [qtts] [app]" >&2; exit 1 ;;
  esac
done

# YOAI_* settings, from the environment and from .env. See .env.example.
# After the arguments, so the help text still prints when a configured path
# is wrong. The names are the paths this script reads: each one that is
# configured has to exist.
yoai_load_env VCPKG_ROOT OPENSSL_ROOT_DIR LLAMA_SRC QTTS_SRC VULKAN_SDK || exit 1

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
      # The toolchain looks for vcpkg.json only in the source directory, which
      # for cmake-js is native/. See the same flags in build.cmd for why both.
      "--CDVCPKG_MANIFEST_DIR=$PROJECT_DIR"
      "--CDVCPKG_MANIFEST_MODE=ON"
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
    third_verify llama.cpp LLAMA_SRC "$LLAMA_SRC"

    LBUILD="$SDK_DIR/llama"
    LSDK="$SDK_DIR/llama-sdk"
    echo "[build] llama.cpp from $LLAMA_SRC"
    # Configured every run, not only when the tree is new. The guard that used
    # to be here saved a few seconds and cost a whole class of bug: a flag added
    # below has no effect on a tree already configured without it, so the build
    # keeps succeeding and the addon keeps failing to link. CMake is idempotent
    # and quick when nothing changed.
    cmake -S "$LLAMA_SRC" -B "$LBUILD" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DLLAMA_BUILD_TESTS=OFF \
      -DLLAMA_BUILD_EXAMPLES=OFF \
      -DLLAMA_BUILD_TOOLS=OFF \
      -DLLAMA_BUILD_SERVER=OFF \
      -DLLAMA_CURL=OFF
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

  # A desktop qwentts.cpp SDK: speech output, with whatever GPU backend this
  # platform has. design/voice.md measures the CPU at RTF 1.13 and a laptop iGPU
  # at 0.63, and below 1 is a threshold rather than a slope -- a stream needs a
  # pre-buffer of duration x (RTF - 1), which at 0.63 is nothing and at 1.13 is
  # eight seconds for a minute of audio. So the backend is the lane.
  if [ -n "$DO_QTTS" ]; then
    QTTS_SRC="${QTTS_SRC:-$PROJECT_DIR/../qwentts.cpp}"
    if [ ! -f "$QTTS_SRC/src/qwen.h" ]; then
      echo "no qwentts.cpp at $QTTS_SRC." >&2
      echo "clone it at the pinned commit, submodule and patches included:" >&2
      echo "  ../third/sync.sh clone qwentts.cpp" >&2
      echo "or point QTTS_SRC at an existing checkout." >&2
      exit 1
    fi
    third_verify qwentts.cpp QTTS_SRC "$QTTS_SRC"

    # ONE ggml for both lanes, and llama.cpp's is the one. qwentts builds a ggml
    # of its own from a fork, and until 2026-09-21 that copy superset llama's,
    # so linking qwentts's satisfied both libraries. The update inverted it --
    # libllama now wants ggml_dsv4_hc_pre_gated, which the fork does not have --
    # and whichever copy is kept, one symbol goes unmet and the addon does not
    # link. GGML_SOURCE_DIR, which qwentts added the same day, points its build
    # at any ggml tree, so both lanes compile the same one from the same commit.
    #
    # The cost is the fork's ggml_compute_forward_snake_fused, a graph pattern
    # match that collapses the DAC decoder's snake activation into one kernel.
    # Same output without it, some unmeasured amount slower, in the codec stage.
    # design/voice.md has the symbol tables.
    LLAMA_SRC="${LLAMA_SRC:-$PROJECT_DIR/../llama.cpp}"
    if [ ! -f "$LLAMA_SRC/ggml/CMakeLists.txt" ]; then
      echo "qtts compiles llama.cpp's ggml and there is none at $LLAMA_SRC/ggml." >&2
      echo "  ../third/sync.sh clone llama.cpp" >&2
      exit 1
    fi
    third_verify llama.cpp LLAMA_SRC "$LLAMA_SRC"

    # The GPU backend is per platform, not optional-per-platform.
    #
    #   macOS   Metal, and ggml turns it on ITSELF -- ggml/CMakeLists.txt does
    #           `if (APPLE) set(GGML_METAL_DEFAULT ON)`. So the right thing here
    #           is to pass nothing, and passing GGML_VULKAN would be wrong:
    #           there is no native Vulkan on macOS.
    #   Linux   Vulkan, the same flag Windows uses. Needs the loader, the headers
    #           and glslc, which ggml compiles its shaders with.
    QTTS_BACKEND=()
    if [ "$(uname -s)" = "Darwin" ]; then
      echo "[build] qtts backend: Metal (ggml enables it for APPLE by itself)"
    else
      if [ -z "${VULKAN_SDK:-}" ] && ! command -v glslc >/dev/null 2>&1; then
        echo "qtts needs Vulkan: the loader, the headers, and glslc." >&2
        echo "  Debian/Ubuntu: apt install libvulkan-dev glslc" >&2
        echo "  Fedora:        dnf install vulkan-loader-devel glslc" >&2
        echo "or set VULKAN_SDK if you installed the LunarG SDK somewhere else." >&2
        exit 1
      fi
      echo "[build] qtts backend: Vulkan"
      QTTS_BACKEND=(-DGGML_VULKAN=ON)
      # SPIRV-Headers named explicitly, because ggml's own CMAKE_PREFIX_PATH
      # append of VULKAN_SDK does not find it and the failure reads like a
      # missing SDK rather than a layout difference. find_package's config
      # search looks under <prefix>/lib/cmake/<name>*/ -- a directory NAMED for
      # the package -- and the LunarG SDK puts SPIRV-HeadersConfig.cmake
      # straight into lib/cmake with no such directory. Distro packages use the
      # upstream layout and need no help, so both are probed and neither
      # present leaves the flag off: ggml's own error names the package and is
      # better than a flag pointing somewhere empty.
      if [ -n "${VULKAN_SDK:-}" ]; then
        for d in "$VULKAN_SDK"/{lib,Lib}/cmake/SPIRV-Headers "$VULKAN_SDK"/{lib,Lib}/cmake; do
          if [ -f "$d/SPIRV-HeadersConfig.cmake" ]; then
            QTTS_BACKEND+=("-DSPIRV-Headers_DIR=$d")
            break
          fi
        done
      fi
    fi

    QBUILD="$SDK_DIR/qtts"
    QSDK="$SDK_DIR/qtts-sdk"
    echo "[build] qwentts.cpp from $QTTS_SRC, ggml from $LLAMA_SRC/ggml"
    cmake -S "$QTTS_SRC" -B "$QBUILD" \
      -DCMAKE_BUILD_TYPE=Release \
      "-DGGML_SOURCE_DIR=$LLAMA_SRC/ggml" \
      -DBUILD_SHARED_LIBS=OFF \
      -DQWEN_SHARED=OFF \
      "${QTTS_BACKEND[@]+"${QTTS_BACKEND[@]}"}"
    # Only qwen-core by name: qwen-tts, qwen-codec and tts-server all build by
    # default and all want a main(), which the addon has no use for.
    cmake --build "$QBUILD" --parallel --target qwen-core

    mkdir -p "$QSDK/include" "$QSDK/lib"
    cp "$QTTS_SRC"/src/qwen*.h "$QSDK/include/"
    # Collected by search rather than by name because ggml splits its archives
    # differently between versions, and the GPU backend adds one more. A missing
    # one surfaces as an undefined symbol at the addon's link step.
    find "$QBUILD" -name 'libqwen-core.*' -o -name 'libggml*.*' \
      | while IFS= read -r lib; do cp "$lib" "$QSDK/lib/"; done
    if ! ls "$QSDK"/lib/libqwen-core.* >/dev/null 2>&1; then
      echo "[build] qwentts built but no qwen-core library landed in $QSDK/lib" >&2
      exit 1
    fi
    echo "[build] qtts SDK at $QSDK"

    CMAKE_JS_ARGS+=(--CDBODYCLOCK_QTTS=ON "--CDQTTS_DIR=$QSDK")
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
  #
  # configure AND build, the same list to both, because `compile` on its own
  # drops every --CD flag on a build directory that already has a cache.
  # cmake-js configures only when there is none, so a lane switched on by an
  # argument to this script stays OFF in the cache -- and NOTHING SAYS SO. The
  # addon still links, still exports the same entry points (see binding.cpp on
  # the tts half) and only answers nativeLocalAvailable() with false at
  # runtime, which reads as a missing model rather than an ignored flag. Same
  # as the "configured every run" note on the two SDK trees above.
  CMAKE_JS_COMMON=(
    --directory "$DESKTOP_DIR/native"
    --out "$BUILD_DIR"
    --runtime electron
    --runtime-version "$ELECTRON_VER"
    --CDCMAKE_BUILD_TYPE=Release
    # The [@]+ dance, and not a plain "${CMAKE_JS_ARGS[@]}", for the bash 3.2
    # that macOS ships: there, `set -u` treats an empty array's [@] as unset,
    # and this array IS empty on a machine with no vcpkg and no lane asked for.
    "${CMAKE_JS_ARGS[@]+"${CMAKE_JS_ARGS[@]}"}"
  )
  for cmd in configure build; do
    node "$DESKTOP_DIR/node_modules/cmake-js/bin/cmake-js" "$cmd" "${CMAKE_JS_COMMON[@]}"
  done

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
