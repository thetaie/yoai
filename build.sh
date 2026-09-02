#!/usr/bin/env bash
# Desktop build: yoai_core plus the CLI harness, into build/.
#
# The point of a desktop build is that the core can be exercised without a
# device -- see cli/main.cpp. It is not a shipping target.
#
# Usage: ./build.sh [clean]
# Env overrides:
#   VCPKG_ROOT   vcpkg checkout (default: a sibling of the repo)
set -euo pipefail

cd "$(dirname "$0")"
PROJECT_DIR="$(pwd)"
BUILD_DIR="$PROJECT_DIR/build"

if [ "${1:-}" = "clean" ]; then
  rm -rf "$BUILD_DIR"
fi

VCPKG_ROOT="${VCPKG_ROOT:-$(cd .. && pwd)/vcpkg}"
if [ ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]; then
  echo "vcpkg not found at $VCPKG_ROOT. Set VCPKG_ROOT." >&2
  exit 1
fi

if [ ! -f "$BUILD_DIR/build.ninja" ] && [ ! -f "$BUILD_DIR/Makefile" ]; then
  cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_INSTALLED_DIR="$PROJECT_DIR/vcpkg_installed"
fi

cmake --build "$BUILD_DIR" --parallel
