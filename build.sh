#!/usr/bin/env bash
# Desktop build: bodyclock_core plus the CLI harness, into build/.
#
# The point of a desktop build is that the core can be exercised without a
# device -- see cli/main.cpp. It is not a shipping target.
#
# Dependencies come from vcpkg when it is there, and from the system's own
# packages when it is not -- see the comment above the vcpkg block below.
#
# Usage: ./build.sh [clean]
# Env overrides:
#   VCPKG_ROOT        vcpkg checkout (default: a sibling of the repo, used only
#                     if it exists)
#   OPENSSL_ROOT_DIR  where to find OpenSSL, when the default search misses it
set -euo pipefail

cd "$(dirname "$0")"
PROJECT_DIR="$(pwd)"
BUILD_DIR="$PROJECT_DIR/build"

if [ "${1:-}" = "clean" ]; then
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

if [ ! -f "$BUILD_DIR/build.ninja" ] && [ ! -f "$BUILD_DIR/Makefile" ]; then
  cmake -S . -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"
fi

cmake --build "$BUILD_DIR" --parallel
