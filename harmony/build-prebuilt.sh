#!/usr/bin/env bash
# Cross-compile the core's native dependencies for HarmonyOS / OpenHarmony and lay
# them out under harmony/prebuilt/<ABI>/ so entry/src/main/cpp/CMakeLists.txt
# (which passes -DCMAKE_PREFIX_PATH) and the repo-root CMakeLists.txt find_package()
# calls resolve. The macOS/Linux counterpart of build-prebuilt.cmd (same vcpkg
# baseline, triplets, and port set).
#
# vcpkg ships OHOS triplets but has taught no *port* about the platform, and the
# pinned tool does not map CMAKE_SYSTEM_NAME=OHOS to its own toolchain. Both are
# worked around by the overlay triplets in harmony/vcpkg-triplets/ -- read the
# comments there before changing anything here.
#
# Re-running is idempotent: vcpkg skips already-built ports and the destination is
# refreshed.
#
# Usage:  ./build-prebuilt.sh [abi]
#   abi defaults to arm64-v8a; one of arm64-v8a|x86_64|armeabi-v7a.
# Env overrides:
#   VCPKG_ROOT      vcpkg checkout (default: a sibling of the repo)
#   OHOS_SDK_ROOT   the SDK dir CONTAINING native/ (default: resolved below)
set -euo pipefail

cd "$(dirname "$0")"
SCRIPT_DIR="$(pwd)"

ABI="${1:-arm64-v8a}"

# READ from vcpkg.json's "builtin-baseline" rather than repeating the hash here:
# two copies of a pin is one copy that silently goes stale.
VCPKG_JSON="$SCRIPT_DIR/../vcpkg.json"
BASELINE="$(sed -n 's/.*"builtin-baseline"[[:space:]]*:[[:space:]]*"\([0-9a-f]\{40\}\)".*/\1/p' "$VCPKG_JSON")"
if [ -z "$BASELINE" ]; then
    echo "no 40-hex \"builtin-baseline\" in \"$VCPKG_JSON\"" >&2
    exit 1
fi

# OHOS ABI -> overlay triplet (harmony/vcpkg-triplets/).
case "$ABI" in
  arm64-v8a)   TRIPLET="arm64-ohos" ;;
  x86_64)      TRIPLET="x64-ohos" ;;
  armeabi-v7a) TRIPLET="arm-ohos" ;;
  *) echo "abi must be arm64-v8a|x86_64|armeabi-v7a (got '$ABI')" >&2; exit 2 ;;
esac

# Default the vcpkg checkout to a sibling of the repo so it is not committed.
VCPKG_ROOT="${VCPKG_ROOT:-$(cd .. && cd .. && pwd)/vcpkg}"

# --- Resolve the OpenHarmony SDK ---------------------------------------------
# OHOS_SDK_ROOT must name the directory CONTAINING native/ (vcpkg's
# scripts/toolchains/ohos.cmake appends /native itself).
if [ -z "${OHOS_SDK_ROOT:-}" ]; then
  for d in \
    "$HOME/.local/opt/ohos/command-line-tools/sdk/default/openharmony" \
    "$HOME/command-line-tools/sdk/default/openharmony" \
    "/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony"
  do
    if [ -f "$d/native/build/cmake/ohos.toolchain.cmake" ]; then OHOS_SDK_ROOT="$d"; break; fi
  done
fi
if [ -z "${OHOS_SDK_ROOT:-}" ] || [ ! -f "$OHOS_SDK_ROOT/native/build/cmake/ohos.toolchain.cmake" ]; then
  echo "OpenHarmony SDK not found. Set OHOS_SDK_ROOT to the directory containing native/." >&2
  exit 1
fi

# --- Space-free SDK path (mandatory) -----------------------------------------
# OpenSSL's generated Makefile hands the compiler path and --sysroot to /bin/sh
# UNQUOTED, so a space anywhere in the SDK path breaks the build. A symlink is
# enough (the Windows script uses a junction for the same reason).
case "$OHOS_SDK_ROOT" in
  *\ *)
    case "$VCPKG_ROOT" in
      *\ *) echo "SDK path contains a space ['$OHOS_SDK_ROOT'] and so does VCPKG_ROOT; point OHOS_SDK_ROOT at a space-free path." >&2; exit 1 ;;
    esac
    SDK_LINK="$VCPKG_ROOT/ohos-sdk"
    if [ ! -f "$SDK_LINK/native/build/cmake/ohos.toolchain.cmake" ]; then
      echo "SDK path has a space; linking a space-free alias: $SDK_LINK"
      rm -f "$SDK_LINK"
      ln -s "$OHOS_SDK_ROOT" "$SDK_LINK"
    fi
    OHOS_SDK_ROOT="$SDK_LINK"
    ;;
esac
export OHOS_SDK_ROOT

echo "ABI=$ABI  triplet=$TRIPLET"
echo "SDK=$OHOS_SDK_ROOT"
echo "vcpkg=$VCPKG_ROOT"
echo "baseline=$BASELINE"

# --- Bootstrap vcpkg at the pinned baseline -----------------------------------
if [ ! -d "$VCPKG_ROOT/.git" ]; then
  echo "Cloning vcpkg ..."
  git clone https://github.com/microsoft/vcpkg "$VCPKG_ROOT"
fi
git -C "$VCPKG_ROOT" checkout "$BASELINE"
if [ ! -x "$VCPKG_ROOT/vcpkg" ]; then
  "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi

# --- Build the core's deps for OHOS (classic mode) ----------------------------
# --classic: this script runs inside the repo, whose vcpkg.json would otherwise put
# vcpkg into manifest mode (which rejects port-name arguments).
#
# zlib is named explicitly even though curl pulls it in: the OHOS triplet builds
# everything static, so the final link needs it resolvable on its own.
PORTS=(openssl "curl[openssl]" rapidjson zlib)
echo "Installing: ${PORTS[*]}  (--triplet $TRIPLET)"
"$VCPKG_ROOT/vcpkg" install "${PORTS[@]}" --classic --triplet "$TRIPLET" \
  --overlay-triplets="$SCRIPT_DIR/vcpkg-triplets" \
  --x-buildtrees-root "$VCPKG_ROOT/buildtrees"

# --- Lay out under harmony/prebuilt/<ABI> -------------------------------------
INSTALLED="$VCPKG_ROOT/installed/$TRIPLET"
[ -d "$INSTALLED" ] || { echo "expected install tree missing: $INSTALLED" >&2; exit 1; }
DEST="$SCRIPT_DIR/prebuilt/$ABI"
rm -rf "$DEST"
mkdir -p "$DEST"
# Copy debug/ too: vcpkg's *Targets.cmake reference both release (lib/) and debug
# (debug/lib/) imported locations, and CMake verifies both exist at find_package time.
for sub in include lib share debug; do
  if [ -d "$INSTALLED/$sub" ]; then
    cp -R "$INSTALLED/$sub" "$DEST/$sub"
  fi
done

echo ""
echo "Done. Prebuilt deps for $ABI -> $DEST"
echo "Now build the app in DevEco, or the native target directly:"
echo "  cmake -B build-ohos -G Ninja \\"
echo "    -DCMAKE_TOOLCHAIN_FILE=$OHOS_SDK_ROOT/native/build/cmake/ohos.toolchain.cmake \\"
echo "    -DOHOS_ARCH=$ABI \\"
echo "    -DCMAKE_FIND_ROOT_PATH=$DEST -DCMAKE_PREFIX_PATH=$DEST \\"
echo "    -DYOAI_BUILD_CLI=OFF"
echo "  cmake --build build-ohos --target yoai_core"
