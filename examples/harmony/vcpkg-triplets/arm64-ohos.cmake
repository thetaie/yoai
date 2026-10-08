# Overlay triplet: OpenHarmony / HarmonyOS NEXT, arm64-v8a.
#
# vcpkg ships triplets/community/arm64-ohos.cmake and scripts/toolchains/ohos.cmake,
# but the pinned vcpkg tool (2026-04-08, see scripts/vcpkg-tool-metadata.txt) does
# not yet map CMAKE_SYSTEM_NAME=OHOS to that toolchain on its own -- it fails with
# "Unable to determine toolchain use for arm64-ohos ... Did you mean to use
# VCPKG_CHAINLOAD_TOOLCHAIN_FILE?". This overlay is the community triplet plus that
# one explicit chainload, so it works with the tool version the repo already pins.
# Drop this file once the pinned tool resolves OHOS natively.
#
# Requires OHOS_SDK_ROOT to point at the SDK root that CONTAINS `native/`, e.g.
#   C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony
# scripts/toolchains/ohos.cmake appends /native and includes the SDK's own
# build/cmake/ohos.toolchain.cmake from there.

set(VCPKG_ENV_PASSTHROUGH_UNTRACKED OHOS_SDK_ROOT)

set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# Linux, not OHOS, and deliberately so. Upstream vcpkg ships OHOS triplets but
# has NOT taught any port about the platform: openssl's Configure dispatch dies
# with "Unknown platform", and the other autotools/Configure-driven ports would
# follow. OHOS *is* Linux (aarch64-linux-ohos, musl-flavoured libc), so claiming
# Linux lets every port take its normal Linux path.
#
# This only changes vcpkg's own notion of the target. The real compiler, sysroot
# and CMAKE_SYSTEM_NAME still come from the chainloaded SDK toolchain below,
# which sets CMAKE_SYSTEM_NAME=OHOS for the actual builds. Revisit once the
# ports themselves handle OHOS.
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-unknown-linux-ohos")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    -DOHOS_ARCH=arm64-v8a
    -DCMAKE_PLATFORM_NO_VERSIONED_SONAME=ON)

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${VCPKG_ROOT_DIR}/scripts/toolchains/ohos.cmake")
