# Overlay triplet: OpenHarmony / HarmonyOS NEXT, armeabi-v7a.
# See arm64-ohos.cmake in this directory for why these three deviations from
# vcpkg's community triplet are needed (chainload, Linux system name, and the
# space-free OHOS_SDK_ROOT requirement).

set(VCPKG_ENV_PASSTHROUGH_UNTRACKED OHOS_SDK_ROOT)

set(VCPKG_TARGET_ARCHITECTURE arm)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=arm-unknown-linux-ohos")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    -DOHOS_ARCH=armeabi-v7a
    -DCMAKE_PLATFORM_NO_VERSIONED_SONAME=ON)

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${VCPKG_ROOT_DIR}/scripts/toolchains/ohos.cmake")
