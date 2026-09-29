#!/usr/bin/env python3
"""Usage: python build.py [abi] [deps|llama|qtts|vulkan|apk|all] [clean]
                       [arch=<march>|baseline]

Everything the Android app needs built natively, before Gradle can compile
the APK. This does NOT build the app itself -- `gradlew assembleDebug`, or
open android/ in Android Studio, for that. It builds what the app links
against.

  abi     arm64-v8a (default) | x86_64
  deps    the core's C++ dependencies via vcpkg -> prebuilt/<abi>/
          (REQUIRED: without it the native module fails to configure on its
          first find_package)
  llama   the on-device LLM engine, and the ggml recognition rides on
          -> prebuilt/llama-sdk/<abi>/
  qtts    speech output (qwentts.cpp) -> prebuilt/qtts-sdk/<abi>/
          Needs a llama.cpp checkout too, whether or not `llama` is being
          built: it compiles llama.cpp's ggml.
  vulkan  modifies qtts: also build ggml's Vulkan backend, for Adreno.
          Implies qtts. Not a target of its own.
  apk     assemble the debug APK via the Gradle wrapper
  all     deps, llama, apk -- and what a bare `python build.py` does
  clean   discard the cached cross-build trees and SDKs first
  arch=   llama/qtts only: the -march feature set; `baseline` means armv8-a

About arch=. design/voice.md measures a cross build with no -march landing on
baseline armv8-a and costing 2.9x prefill and 2.2x decode, because CMake's
feature probes compile AND RUN a test binary and cannot run one for the phone.
So the feature set is asserted here rather than detected. The default is the
HarmonyOS lane's, which is what a Kirin 9020 has; a current Qualcomm core is
armv9.2-a and has more. The Device panel in the app prints what the chip
actually reports, which is how the assertion gets checked -- set arch= to
match it, and re-measure.

Environment:
  ANDROID_SDK_ROOT  the SDK. Default: ANDROID_HOME, then sdk.dir in
                    android/local.properties
  ANDROID_NDK_ROOT  the NDK (default: the version in android/gradle.properties)
These also read as YOAI_<NAME>, from the environment or from .env at the
repository root, which wins -- see .env.example:
  VCPKG_ROOT        vcpkg checkout (default: a sibling of the repo)
  LLAMA_SRC         llama.cpp checkout (default: a sibling of the repo)
  QTTS_SRC          qwentts.cpp checkout (default: a sibling of the repo)

`python ../third/sync.py clone` fetches the checkouts at their pinned
commits, with the patches this script expects already applied.
"""

# The Android counterpart of harmony/build.py, and deliberately the same shape:
# same tokens, same prebuilt/<abi> layout, same rule that BUILDING AN SDK IS
# TURNING ITS LANE ON. What it is not is a port of that script's workarounds.
# The OHOS lane patches ggml because musl hides cpu_set_t behind _GNU_SOURCE
# and has no pthread_setaffinity_np at all; bionic needs neither, and ggml's
# affinity branch already names Android, so that patch does not apply here.
#
# Python 3.8 or newer, standard library only.

import glob
import importlib.util
import os
import sys

# What every build script shares is in the root build.py. Loaded by path, under
# a name of its own: `import build` from here would import this file.
sys.dont_write_bytecode = True  # no __pycache__ in the repository
_spec = importlib.util.spec_from_file_location(
    "yoai_build", os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build.py"))
b = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(b)
say, fail, run = b.say, b.fail, b.run

HERE = os.path.dirname(os.path.abspath(__file__))
# The Kirin 9020's feature set, which is where every number in design/voice.md
# comes from. Kept as the default so a build on a new device is comparable
# against those before it is optimised for the new one.
DEFAULT_ARCH = "armv8.6-a+i8mm+bf16+dotprod+fp16"
# The API level the NDK targets. 26 matches minSdk in app/build.gradle.kts; a
# mismatch is a link error naming a libc symbol rather than a configuration.
API_LEVEL = 26
TRIPLETS = {"arm64-v8a": "arm64-android", "x86_64": "x64-android"}


class Opts:
    abi = ""
    deps = llama = qtts = vulkan = apk = clean = False
    arch = None  # None: not given, so the default


def parse(argv):
    """Position-independent. An unrecognized token is an ERROR rather than a
    guess, for the reason harmony/build.py records: its predecessor treated
    any unknown token as the -march string, so a mistyped target silently
    became `-march=lama`."""
    o = Opts()
    for tok in argv:
        t = tok.lower()
        if t in ("help", "-h", "--help", "/?"):
            say(__doc__.strip())
            raise b.Stop(0)
        elif t == "deps":
            o.deps = True
        elif t == "llama":
            o.llama = True
        elif t == "qtts":
            o.qtts = True
        elif t == "vulkan":
            o.qtts = o.vulkan = True
        elif t == "apk":
            o.apk = True
        elif t == "all":
            o.deps = o.llama = o.apk = True
        elif t == "clean":
            o.clean = True
        elif t == "baseline":
            o.arch = ""
        elif t.startswith("arch="):
            o.arch = tok[5:]
        elif not o.abi:
            o.abi = t
        else:
            fail('[build] unknown argument "%s". Try: %s --help' % (tok, b.script_cmd(__file__)), code=2)
    return o


def main(argv):
    o = parse(argv)
    if not b.load_env("VCPKG_ROOT", "LLAMA_SRC", "QTTS_SRC"):
        return 1

    o.abi = o.abi or "arm64-v8a"
    # NOTHING named means none of the targets, not none of the three `all`
    # names: harmony's script once tested only three, so naming one it had
    # left out read as an empty command line and quietly built everything.
    if not (o.deps or o.llama or o.qtts or o.apk):
        o.deps = o.llama = o.apk = True
    if o.abi not in TRIPLETS:
        fail('[build] unsupported abi "%s" (arm64-v8a, x86_64)' % o.abi, code=2)
    arch = DEFAULT_ARCH if o.arch is None else o.arch
    if o.abi == "x86_64":
        arch = ""

    sdk_root, ndk_root = android_sdk()
    toolchain = os.path.join(ndk_root, "build", "cmake", "android.toolchain.cmake")
    if not os.path.isfile(toolchain):
        fail('[build] no toolchain file at "%s"' % toolchain)
    cmake, ninja = cmake_ninja(sdk_root)

    vcpkg_root = b.env("VCPKG_ROOT", b.sibling("vcpkg"))
    llama_src = b.env("LLAMA_SRC", b.sibling("llama.cpp"))
    qtts_src = b.env("QTTS_SRC", b.sibling("qwentts.cpp"))

    say("[build] abi=%s  api=%d  arch=%s" % (o.abi, API_LEVEL, arch or "baseline"))
    say("[build] ndk=%s" % ndk_root)

    if o.clean:
        say("[build] cleaning cross-build trees")
        for d in (".llama-build", ".qtts-build", os.path.join("prebuilt", "llama-sdk"),
                  os.path.join("prebuilt", "qtts-sdk")):
            b.rmtree(os.path.join(HERE, d, o.abi))

    # Shared by both engine builds. ANDROID_ABI/PLATFORM are what the NDK
    # toolchain file reads; the rest is this project's policy.
    #
    # ANDROID_STL matches app/build.gradle.kts rather than taking the NDK's
    # default of c++_static. For static archives the two are nearly the same
    # thing -- an .a carries undefined references to libc++ either way, and it
    # is the final shared library that decides which one resolves them -- so
    # this is about not having to re-derive that every time the question
    # comes up.
    ndk_cfg = ["-G", "Ninja",
               "-DCMAKE_MAKE_PROGRAM=" + ninja,
               "-DCMAKE_TOOLCHAIN_FILE=" + toolchain,
               "-DANDROID_ABI=" + o.abi,
               "-DANDROID_PLATFORM=android-%d" % API_LEVEL,
               "-DANDROID_STL=c++_shared",
               "-DCMAKE_BUILD_TYPE=Release"]

    if o.deps:
        # No overlay triplet, unlike the OHOS lane: vcpkg ships arm64-android
        # and x64-android, and they are already static. The NDK reaches vcpkg's
        # port builds through the environment.
        os.environ["ANDROID_NDK_HOME"] = ndk_root
        b.vcpkg_deps(vcpkg_root, TRIPLETS[o.abi], os.path.join(HERE, "prebuilt", o.abi))
    if o.llama:
        build_llama(o, cmake, ndk_cfg, arch, llama_src)
    if o.qtts:
        build_qtts(o, cmake, ndk_cfg, arch, llama_src, qtts_src)
    if o.apk:
        say("[apk] assembleDebug")
        # Through sh off Windows: git records gradlew as 644, so a fresh clone
        # on Linux or macOS has no execute bit on it.
        wrapper = [os.path.join(HERE, "gradlew.bat")] if b.WINDOWS else ["sh", os.path.join(HERE, "gradlew")]
        run(wrapper + ["assembleDebug", "-Pyoai.abi=" + o.abi], cwd=HERE)
        say("[apk] done -> %s" % os.path.join(HERE, "app", "build", "outputs", "apk", "debug"))
    say()
    say("[build] done.")
    return 0


# -----------------------------------------------------------------------------
# Toolchain discovery
# -----------------------------------------------------------------------------
def android_sdk():
    """The SDK and the NDK in it. Three sources for the SDK, in the order of
    how explicitly each one was chosen. ANDROID_HOME is here because it is the
    variable the SDK's own installers and Android Studio still set -- looking
    only at ANDROID_SDK_ROOT means a machine with a perfectly good SDK reports
    not having one."""
    sdk = b.env("ANDROID_SDK_ROOT") or b.env("ANDROID_HOME")
    if not sdk:
        # local.properties is the file Gradle already reads, so there is one
        # place the SDK path lives rather than two that can disagree. Gradle
        # escapes what it writes there -- `C\:\\Users\\me\\...` -- so it is
        # unescaped back into a path before anything tries to open it.
        for line in b.read(os.path.join(HERE, "local.properties")).splitlines():
            if line.startswith("sdk.dir="):
                sdk = line[len("sdk.dir="):].strip().replace("\\\\", "\\").replace("\\:", ":")
                break
    if not sdk or not os.path.isdir(sdk):
        fail('[build] no Android SDK at "%s".' % (sdk or "<unset>"),
             "[build] set ANDROID_HOME or ANDROID_SDK_ROOT, or put sdk.dir in",
             "[build]   %s" % os.path.join(HERE, "local.properties"))

    ndk = b.env("ANDROID_NDK_ROOT")
    if not ndk:
        version = ""
        for line in b.read(os.path.join(HERE, "gradle.properties")).splitlines():
            if line.startswith("yoai.ndk="):
                version = line[len("yoai.ndk="):].strip()
                break
        if not version:
            fail('[build] no yoai.ndk in "%s"' % os.path.join(HERE, "gradle.properties"))
        ndk = os.path.join(sdk, "ndk", version)
    if not os.path.isdir(ndk):
        fail('[build] no NDK at "%s"' % ndk)
    return sdk, ndk


def cmake_ninja(sdk):
    """The SDK's cmake and ninja, so the version is the one
    app/build.gradle.kts pins rather than whatever is on PATH."""
    found = {}
    for d in sorted(glob.glob(os.path.join(sdk, "cmake", "*", "bin"))):
        for tool in ("cmake", "ninja"):
            path = os.path.join(d, tool + b.EXE)
            if tool not in found and os.path.isfile(path):
                found[tool] = path
    cmake = found.get("cmake") or b.which("cmake")
    ninja = found.get("ninja") or b.which("ninja")
    if not cmake or not ninja:
        fail("[build] need cmake and ninja (the SDK's cmake/*/bin, or on PATH)")
    return cmake, ninja


# -----------------------------------------------------------------------------
# llama -- the on-device LLM, and the ggml that recognition links
# -----------------------------------------------------------------------------
def build_llama(o, cmake, ndk_cfg, arch, src):
    if not os.path.isdir(src):
        fail('[llama] no llama.cpp at "%s".' % src,
             "[llama]   %s   (pinned commit, patch applied)" % b.sync_cmd("clone", "llama.cpp"))
    b.third_verify("llama.cpp", "LLAMA_SRC", src)

    lbuild = os.path.join(HERE, ".llama-build", o.abi)
    sdk = os.path.join(HERE, "prebuilt", "llama-sdk", o.abi)

    # GGML_OPENMP=OFF, and this is not just a link fix.
    #
    # ggml turns OpenMP on wherever find_package(OpenMP) succeeds. It fails on
    # the OHOS sysroot, which has no libomp, so every figure in design/voice.md
    # was taken on **ggml's own thread pool**. The NDK does ship libomp, so the
    # same source would quietly run a different scheduler here and a
    # thread-count sweep would not be comparable with the Kirin's -- which is
    # most of what this lane exists to compare.
    #
    # It also fails to link if left on: the archives reference __kmpc_* and
    # nothing pulls the runtime into libyoai.so.
    cfg = ndk_cfg + ["-S", src, "-B", lbuild,
                     "-DBUILD_SHARED_LIBS=OFF",
                     "-DLLAMA_CURL=OFF",
                     "-DLLAMA_BUILD_TESTS=OFF",
                     "-DLLAMA_BUILD_EXAMPLES=OFF",
                     "-DLLAMA_BUILD_SERVER=OFF",
                     "-DLLAMA_BUILD_TOOLS=OFF",
                     "-DGGML_OPENMP=OFF"]
    if arch:
        cfg.append("-DGGML_CPU_ARM_ARCH=" + arch)
    run([cmake] + cfg)
    # Only the libraries: upstream's CLI target links implementation libs the
    # OFF switches above remove, so asking for it would fail the link.
    run([cmake, "--build", lbuild, "--target", "llama", "ggml", "ggml-cpu", "ggml-base"])

    say("[llama] assembling %s" % sdk)
    inc, libdir = os.path.join(sdk, "include"), os.path.join(sdk, "lib")
    b.mkdirs(inc, libdir)
    b.copy_headers([os.path.join(src, "include", "llama.h")], inc)
    b.copy_headers_glob(os.path.join(src, "ggml", "include", "*.h"), inc)
    # Every archive is checked before any is copied, so a partial SDK stops the
    # script rather than being discovered later as a link error.
    libs = [os.path.join(lbuild, "src", "libllama.a"),
            os.path.join(lbuild, "ggml", "src", "libggml.a"),
            os.path.join(lbuild, "ggml", "src", "libggml-cpu.a"),
            os.path.join(lbuild, "ggml", "src", "libggml-base.a")]
    for lib in libs:
        if not os.path.isfile(lib):
            fail("[llama] missing %s" % lib)
    b.copy_all(libs, libdir)
    say("[llama] done -> %s" % sdk)


# -----------------------------------------------------------------------------
# qtts -- speech output (qwentts.cpp), optionally with ggml's Vulkan backend
# -----------------------------------------------------------------------------
def build_qtts(o, cmake, ndk_cfg, arch, llama_src, src):
    if not os.path.isdir(src):
        fail('[qtts] no qwentts.cpp at "%s".' % src,
             "[qtts]   " + b.sync_cmd("clone", "qwentts.cpp"))

    # third/patches/ has two for this checkout. qwentts-ggml-ohos-affinity.patch
    # is for musl's missing pthread_setaffinity_np and does NOT apply here --
    # bionic has it. The thread-count one is not platform-specific: qwentts.cpp
    # picks hardware_concurrency()/2 reasoning about hyperthreads a phone does
    # not have. It is applied through the SAME patch file as the other lanes,
    # so they cannot drift into overriding the thread count differently.
    b.qpatch(src, "qwentts-thread-count-override.patch")

    # ONE ggml for both lanes, and llama.cpp's is the one. This used to compile
    # qwentts's own ggml submodule and the app linked that copy for everything,
    # because it superset llama's. The 2026-09-21 update inverted it: libllama
    # wants ggml_dsv4_hc_pre_gated, which the fork does not have, so whichever
    # copy is kept one symbol goes unmet and the app does not link.
    # GGML_SOURCE_DIR, added upstream the same day, points the qwentts build at
    # any ggml tree -- so both lanes compile the same one from the same commit.
    # The cost is the fork's fused snake kernel for the DAC decoder; see
    # design/voice.md. This target therefore needs a llama.cpp checkout even
    # when `llama` is not being built.
    if not os.path.isfile(os.path.join(llama_src, "ggml", "CMakeLists.txt")):
        fail('[qtts] qtts compiles llama.cpp\'s ggml and there is none at "%s".'
             % os.path.join(llama_src, "ggml"),
             "[qtts]   " + b.sync_cmd("clone", "llama.cpp"))
    say("[qtts] ggml=%s" % os.path.join(llama_src, "ggml"))
    b.third_verify("llama.cpp", "LLAMA_SRC", llama_src)
    b.third_verify("qwentts.cpp", "QTTS_SRC", src)

    qbuild = os.path.join(HERE, ".qtts-build", o.abi)
    sdk = os.path.join(HERE, "prebuilt", "qtts-sdk", o.abi)
    cfg = ndk_cfg + ["-S", src, "-B", qbuild,
                     "-DGGML_SOURCE_DIR=" + b.fwd(llama_src) + "/ggml",
                     "-DBUILD_SHARED_LIBS=OFF",
                     # Same reason as the llama lane's: this ggml has to
                     # schedule threads the way the one design/voice.md
                     # measured did.
                     "-DGGML_OPENMP=OFF"]
    if arch:
        cfg.append("-DGGML_CPU_ARM_ARCH=" + arch)
    if o.vulkan:
        # The reason this is worth a target at all: design/voice.md measures
        # ggml's Vulkan backend at RTF 0.60 on a laptop iGPU against 1.13 on its
        # CPU, and the HarmonyOS attempt died to a platform policy
        # (GPU_LEAK_KILL) rather than to anything about the backend. Android has
        # no such policy, so Adreno is the first real chance to find out.
        cfg.append("-DGGML_VULKAN=ON")
    run([cmake] + cfg)
    run([cmake, "--build", qbuild])

    say("[qtts] assembling %s" % sdk)
    inc, libdir = os.path.join(sdk, "include"), os.path.join(sdk, "lib")
    b.mkdirs(inc, libdir)
    # qwen.h is what app/src/main/cpp/CMakeLists.txt probes for, so it is the
    # marker that says this SDK is complete.
    b.copy_headers_glob(os.path.join(src, "src", "qwen*.h"), inc)
    # From LLAMA_SRC, not QTTS_SRC: the headers have to be the ones the
    # archives below were compiled from, and GGML_SOURCE_DIR moved that.
    b.copy_headers_glob(os.path.join(llama_src, "ggml", "include", "*.h"), inc)
    # Collected by search rather than by name because ggml splits its archives
    # differently between versions, and the Vulkan backend adds one more.
    b.copy_all(b.find(qbuild, "libqwen-core.a", "libggml*.a"), libdir)
    if not os.path.isfile(os.path.join(inc, "qwen.h")):
        fail('[qtts] no qwen.h landed in "%s" -- the layout moved' % inc)
    if not os.path.isfile(os.path.join(libdir, "libqwen-core.a")):
        fail('[qtts] qwentts built but libqwen-core.a is not in "%s"' % libdir)
    say("[qtts] done -> %s" % sdk)
    if o.vulkan:
        say("[qtts] Vulkan backend included; the core links it when libggml-vulkan.a is present")


if __name__ == "__main__":
    b.entry(main)
