#!/usr/bin/env python3
"""Usage: python build.py [abi] [deps|llama|mnn|qtts|vulkan|hap|all] [clean]
                       [arch=<march>|baseline]

Everything the HarmonyOS app needs built natively, before DevEco can compile
the HAP. This does NOT build the app itself -- open examples/harmony/ in DevEco for
that, or name `hap`. It builds what the app links against.

  abi     arm64-v8a (default) | x86_64 | armeabi-v7a
  deps    the core's C++ dependencies via vcpkg -> prebuilt/<abi>/
          (REQUIRED: without it the native module fails to configure on its
          first find_package)
  llama   the on-device LLM engine  -> prebuilt/llama-sdk/<abi>/
          (building it is what turns on-device chat on)
  mnn     MNN with the omni Talker -> prebuilt/mnn-sdk/<abi>/
          An EVALUATION lane, deliberately NOT part of `all`: nothing in the
          app links it yet.
  qtts    speech output (qwentts.cpp) -> prebuilt/qtts-sdk/<abi>/
          Needs a llama.cpp checkout too, whether or not `llama` is being
          built: it compiles llama.cpp's ggml.
  vulkan  modifies qtts: also build ggml's Vulkan backend. Implies qtts, and
          is not a target of its own.
  hap     compile and sign the app itself, via hvigor. No IDE needed.
  all     deps, llama, hap -- and what a bare `python build.py` does
  clean   discard the cached build trees and SDKs of the targets named
          first. `deps` always replaces its own output, so there is nothing
          for it to clean.
  arch=   llama/qtts only: the -march feature set; `baseline` means plain
          armv8-a.

The abi defaults to arm64-v8a, which is the only one a HarmonyOS NEXT device
has -- so it is normally left off entirely. Naming no target means `all`, so
the first run is just `python build.py`, and after that `python build.py hap`.

On-device chat is part of the app rather than an opt-in lane, so the bare
command produces an app that has it. That is not free: it needs a llama.cpp
checkout (~200 MB, which this script will not clone for you) plus a few
minutes of cross-compile. To build the app WITHOUT the engine, name the two
targets you do want: python build.py deps hap

Environment, each also as YOAI_<NAME> in the environment or in .env at the
repository root, which wins -- see .env.example:
  DEVECO_HOME     DevEco Studio / command-line-tools install (default: read
                  from DevEco's own .home marker, else common locations)
  VCPKG_ROOT      vcpkg checkout (default: a sibling of the repo)
  OHOS_SDK_ROOT   the SDK dir CONTAINING native/ (default: from DEVECO_HOME)
  LLAMA_SRC       llama.cpp checkout (default: a sibling of the repo).
                  Read by `llama` AND by `qtts`, which compiles its ggml.
  LLAMA_SDK_DIR   where to assemble the llama SDK (default: auto-detected)
  MNN_SRC         MNN checkout (default: a sibling of the repo), `mnn` only
  QTTS_SRC        qwentts.cpp checkout (default: a sibling), `qtts` only
  VULKAN_SDK      a host Vulkan SDK, required by `vulkan`

`python ../../third/sync.py clone` fetches all of these at their pinned commits,
with the patches this script expects already applied.
"""

# Python 3.8 or newer, standard library only.

import glob
import importlib.util
import os
import subprocess
import sys

# What every build script shares is in the root build.py. Loaded by path, under
# a name of its own: `import build` from here would import this file.
sys.dont_write_bytecode = True  # no __pycache__ in the repository
_spec = importlib.util.spec_from_file_location(
    "yoai_build", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build.py"))
b = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(b)
say, fail, run = b.say, b.fail, b.run

HERE = os.path.dirname(os.path.abspath(__file__))
# The feature set of a current arm64 phone SoC. `+sve` is deliberately absent:
# on 128-bit-vector parts the SVE kernels displace the i8mm SMMLA ones without
# a width advantage, which measured SLOWER on prefill. Pass arch=<...> to try
# something else, or `baseline` to compile plain armv8-a.
DEFAULT_ARCH = "armv8.6-a+i8mm+bf16+dotprod+fp16"
# OHOS ABI -> vcpkg overlay triplet (vcpkg-triplets/). Rejected at the
# argument rather than at -DOHOS_ARCH, where an unknown ABI fails deep inside
# the toolchain file with nothing pointing back at the argument.
TRIPLETS = {"arm64-v8a": "arm64-ohos", "x86_64": "x64-ohos", "armeabi-v7a": "arm-ohos"}


class Opts:
    abi = ""
    deps = llama = mnn = qtts = vulkan = hap = clean = False
    arch = None  # None: not given, so the default


def parse(argv):
    """Position-independent. Every token is a keyword or the ABI, and an
    unrecognized one is an ERROR rather than a guess. The predecessor treated
    any unknown token as llama's -march string, so a mistyped target silently
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
        elif t == "mnn":
            o.mnn = True
        elif t == "qtts":
            o.qtts = True
        elif t == "vulkan":
            # `vulkan` only modifies qtts -- it is not a target of its own, and
            # it implies one, so `build.py vulkan` does the useful thing rather
            # than nothing.
            o.qtts = o.vulkan = True
        elif t == "hap":
            o.hap = True
        elif t == "all":
            o.deps = o.llama = o.hap = True
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
    # After the arguments, so the help text still prints when a configured
    # path is wrong. The names are the paths this script reads.
    if not b.load_env("DEVECO_HOME", "VCPKG_ROOT", "OHOS_SDK_ROOT", "LLAMA_SRC", "MNN_SRC",
                      "QTTS_SRC", "VULKAN_SDK"):
        return 1

    o.abi = o.abi or "arm64-v8a"
    # Nothing named: everything, in the order `all` would. The bare command is
    # what a fresh clone runs, and it should end with an installable app that
    # has on-device chat -- not with a half-built tree needing two more
    # invocations nobody mentioned.
    #
    # NOTHING named means none of the five, not none of the three. The guard
    # used to test deps/llama/hap only, so `qtts` -- a target it does not list
    # -- read as an empty command line and quietly built deps, llama and the
    # HAP as well. Same for `mnn`, the lane this file calls opt-in.
    implied_all = not (o.deps or o.llama or o.mnn or o.qtts or o.hap)
    if implied_all:
        o.deps = o.llama = o.hap = True

    if o.abi not in TRIPLETS:
        fail('[build] abi must be arm64-v8a|x86_64|armeabi-v7a (got "%s")' % o.abi, code=2)
    # Refusing outright is right when llama was ASKED for, and wrong when it
    # was merely implied by a bare command line -- on an ABI that cannot have
    # the engine, `build.py armeabi-v7a` should still build everything else.
    if o.llama and o.abi == "armeabi-v7a":
        if not implied_all:
            fail('[build] the on-device engine is built for arm64-v8a|x86_64 only (got "%s")' % o.abi,
                 code=2)
        say("[build] llama skipped: the on-device engine is built for arm64-v8a|x86_64 only")
        o.llama = False

    deveco = deveco_home()
    sdk = ohos_sdk(deveco)
    vcpkg_root = b.env("VCPKG_ROOT", b.sibling("vcpkg"))

    say("[build] abi=%s" % o.abi)
    say("[build] sdk=%s" % sdk)
    for on, line in ((o.deps, "deps  (vcpkg -> prebuilt/%s)" % o.abi),
                     (o.llama, "llama (llama.cpp -> prebuilt/llama-sdk/%s)" % o.abi),
                     (o.mnn, "mnn   (MNN -> prebuilt/mnn-sdk/%s)" % o.abi),
                     (o.qtts, "qtts  (qwentts.cpp -> prebuilt/qtts-sdk/%s)" % o.abi),
                     (o.hap, "hap   (hvigor -> entry/build/default/outputs)")):
        if on:
            say("[build] target: " + line)
    say()

    if o.deps:
        sdk = space_free(sdk, vcpkg_root)
        b.vcpkg_deps(vcpkg_root, TRIPLETS[o.abi], os.path.join(HERE, "prebuilt", o.abi),
                     overlay=os.path.join(HERE, "vcpkg-triplets"))
    if o.llama:
        build_llama(o, sdk)
    if o.mnn:
        build_mnn(o, sdk)
    if o.qtts:
        build_qtts(o, sdk)
    if o.hap:
        build_hap(deveco)

    say()
    if o.hap:
        say("[build] done.")
    else:
        # The abi is named back only when it is NOT the default, so the hint on
        # the common path is the command someone would actually type.
        hint = "" if o.abi == "arm64-v8a" else o.abi + " "
        say("[build] done. Run `%s %shap` to package the app, or open" % (b.script_cmd(__file__), hint))
        say("[build] examples/harmony/ in DevEco Studio.")
    return 0


# -----------------------------------------------------------------------------
# DevEco Studio and the OpenHarmony SDK
# -----------------------------------------------------------------------------
def deveco_home():
    """DevEco Studio, or the command-line tools. Everything below comes out of
    that one install: the NDK for the native halves, and node + hvigor for
    `hap`.

    Found by reading the `.home` file DevEco leaves in its per-version config
    directory, which holds the install path as one line. That beats guessing
    at install locations -- this repo's own author installs to
    D:\\Huawei\\DevEco Studio, which no hardcoded list would have found. The
    newest version's marker wins."""
    home = b.env("DEVECO_HOME")
    if home:
        return home
    if b.WINDOWS:
        config = [os.path.join(b.env("LOCALAPPDATA"), "Huawei")]
    else:
        config = [os.path.expanduser("~/Library/Application Support/Huawei"),
                  os.path.expanduser("~/.config/Huawei")]
    markers = []
    for d in config:
        markers += sorted(glob.glob(os.path.join(d, "DevEcoStudio*", ".home")), reverse=True)
    for marker in markers:
        lines = b.read(marker).lstrip("﻿").splitlines()
        if lines and os.path.isdir(os.path.join(lines[0].strip(), "tools")):
            return lines[0].strip()

    # Last resort, for a layout with no .home to read.
    if b.WINDOWS:
        guesses = [os.path.join(b.env("ProgramFiles"), "Huawei", "DevEco Studio"),
                   os.path.join(b.env("LOCALAPPDATA"), "Huawei", "DevEco Studio")]
    else:
        guesses = ["/Applications/DevEco-Studio.app/Contents",
                   os.path.expanduser("~/.local/opt/ohos/command-line-tools"),
                   os.path.expanduser("~/command-line-tools")]
    for d in guesses:
        if os.path.isdir(os.path.join(d, "tools")) or os.path.isdir(os.path.join(d, "sdk")):
            return d
    return ""


def ohos_sdk(deveco):
    """OHOS_SDK_ROOT, which must name the directory CONTAINING native/ (vcpkg's
    scripts/toolchains/ohos.cmake appends /native itself). Exported, because
    vcpkg's OHOS toolchain reads it from the environment of the port builds
    it spawns, not from anything passed on the command line."""
    sdk = b.env("OHOS_SDK_ROOT")
    if not sdk:
        candidates = []
        if deveco:
            candidates.append(os.path.join(deveco, "sdk", "default", "openharmony"))
        if b.WINDOWS:
            candidates.append(os.path.join(b.env("LOCALAPPDATA"), "OpenHarmony", "Sdk",
                                           "default", "openharmony"))
        for d in candidates:
            if os.path.isfile(toolchain_file(d)):
                sdk = d
                break
    if not sdk:
        fail("[build] OpenHarmony SDK not found. Set OHOS_SDK_ROOT to the directory containing native/,",
             "[build] or DEVECO_HOME to the DevEco Studio / command-line-tools install.")
    if not os.path.isfile(toolchain_file(sdk)):
        fail('[build] No native/build/cmake/ohos.toolchain.cmake under "%s".' % sdk)
    os.environ["OHOS_SDK_ROOT"] = sdk
    return sdk


def toolchain_file(sdk):
    return os.path.join(sdk, "native", "build", "cmake", "ohos.toolchain.cmake")


def space_free(sdk, vcpkg_root):
    """The SDK under a path without spaces, for `deps`.

    OpenSSL's generated Makefile hands the compiler path and --sysroot to
    /bin/sh UNQUOTED, so a space anywhere in the SDK path breaks the build
    with "/bin/sh: .../Huawei/DevEco: No such file or directory". An alias --
    a directory junction on Windows, a symlink elsewhere -- is enough.

    Only `deps` needs this -- llama.cpp is a plain CMake/Ninja build that
    quotes its paths -- but the alias is used for the rest of the run, which is
    harmless: it points at the same SDK."""
    if " " not in sdk:
        return sdk
    if " " in vcpkg_root:
        fail('[deps] SDK path contains a space ["%s"] and so does VCPKG_ROOT;' % sdk,
             "[deps] point OHOS_SDK_ROOT at an SDK under a space-free path.")
    link = os.path.join(vcpkg_root, "ohos-sdk")
    if not os.path.isfile(toolchain_file(link)):
        say("[deps] SDK path has a space; linking a space-free alias: %s" % link)
        b.mkdirs(vcpkg_root)
        # A stale alias is removed as a link, never as a tree: deleting
        # through it would delete the SDK.
        if os.path.lexists(link):
            try:
                if b.WINDOWS:
                    os.rmdir(link)  # a junction is a directory to Windows
                else:
                    os.remove(link)
            except OSError as e:
                fail("[deps] could not remove the old alias %s: %s" % (link, e))
        if b.WINDOWS:
            ok = subprocess.run(["cmd", "/d", "/c", "mklink", "/J", link, sdk],
                                stdout=subprocess.DEVNULL).returncode == 0
        else:
            try:
                os.symlink(sdk, link)
                ok = True
            except OSError:
                ok = False
        if not ok:
            fail("[deps] could not create the alias %s" % link)
    os.environ["OHOS_SDK_ROOT"] = link
    return link


def tools(sdk, tag):
    """The toolchain file, cmake and ninja. The SDK ships its own cmake and
    ninja beside the toolchain; the host's are the fall back."""
    native = os.path.join(sdk, "native")
    found = []
    for tool in ("cmake", "ninja"):
        path = os.path.join(native, "build-tools", "cmake", "bin", tool + b.EXE)
        path = path if os.path.isfile(path) else b.which(tool)
        if not path:
            fail("[%s] need %s (the SDK's build-tools/cmake/bin, or on PATH)" % (tag, tool))
        found.append(path)
    return native, toolchain_file(sdk), found[0], found[1]


def cross(src, build, toolchain, ninja, abi):
    """The configure arguments every cross build here starts with."""
    return ["-S", src, "-B", build, "-G", "Ninja",
            "-DCMAKE_MAKE_PROGRAM=" + ninja,
            "-DCMAKE_TOOLCHAIN_FILE=" + toolchain,
            "-DOHOS_ARCH=" + abi,
            "-DCMAKE_BUILD_TYPE=Release"]


# -----------------------------------------------------------------------------
# The on-device LLM engine: llama.cpp cross-compiled for this ABI.
#
# WHY THE -march FLAGS: a cross build with no -march silently lands on baseline
# armv8-a. CMake's feature probes compile AND RUN a test binary, which is
# impossible when cross-compiling, so HAVE_MATMUL_INT8 /
# HAVE_FP16_VECTOR_ARITHMETIC / HAVE_SVE all "fail" and ggml quietly drops its
# fast kernels -- leaving dotprod, i8mm and fp16 unused on a phone that has all
# three. GGML_CPU_ARM_ARCH states the target's features instead of probing.
#
# CPU ONLY, deliberately -- there is no backend option. A Vulkan build was
# measured on this class of device and lost on every axis (roughly 5x slower
# load, 24x slower prefill, 3.5x slower decode) while adding tens of MB of
# SPIR-V to the HAP. The structural reason is that offload copies every tensor
# into device memory that shares its bandwidth with the CPU, so there is no win
# to be had; it is not a tuning problem.
#
# Output: prebuilt/llama-sdk/<abi>/{include,lib}, which
# entry/src/main/cpp/CMakeLists.txt finds BY ITSELF -- the same contract as the
# vcpkg deps, and deliberately under the same parent. Build it and the
# on-device lane is on; delete it and the core compiles its stub and the app
# still builds. Nothing to paste into build-profile.json5. Static archives: a
# HAP has nowhere to put a companion .so.
# -----------------------------------------------------------------------------
def build_llama(o, sdk):
    arch = DEFAULT_ARCH if o.arch is None else o.arch

    # Not cloned for you, unlike vcpkg: this build wants a checkout you can pin
    # and re-measure against, not one silently refreshed underneath it.
    src = b.env("LLAMA_SRC", b.sibling("llama.cpp"))
    if not os.path.isfile(os.path.join(src, "include", "llama.h")):
        fail('[llama] no llama.cpp at "%s".' % src,
             "[llama] clone it at the pinned commit, with its patch applied:",
             "[llama]   " + b.sync_cmd("clone", "llama.cpp"),
             "[llama] or set LLAMA_SRC to an existing checkout.")
    say("[llama] arch=%s" % (arch or "baseline armv8-a"))
    say("[llama] src=%s" % src)

    # The OHOS affinity patch, which this lane needs and never used to apply.
    # It was in the checkout by hand, undocumented, for long enough that
    # nothing here knew it was load-bearing -- found only when third/ went to
    # record what these trees contain. Now it is a file and this is the line
    # that applies it.
    b.qpatch(src, "llama-ggml-ohos-affinity.patch")
    b.third_verify("llama.cpp", "LLAMA_SRC", src)

    _native, toolchain, cmake, ninja = tools(sdk, "llama")
    lbuild = os.path.join(HERE, ".llama-build", o.abi)
    # In-tree under prebuilt/, for the same reason the vcpkg deps are: the
    # app's CMakeLists resolves it by repo-relative path, so there is no
    # machine-local absolute path to paste into a tracked file.
    default_lsdk = os.path.join(HERE, "prebuilt", "llama-sdk", o.abi)
    lsdk = b.env("LLAMA_SDK_DIR", default_lsdk)

    if o.clean:
        say("[llama] cleaning")
        b.rmtree(lbuild)
        b.rmtree(lsdk)

    # GGML_NATIVE=OFF because probing the host says nothing about the target.
    # LLAMA_CURL=OFF: the app has no model downloader, and curl here would drag
    # in a second, differently-configured copy of a dependency the core
    # already links.
    cfg = cross(src, lbuild, toolchain, ninja, o.abi) + [
        "-DBUILD_SHARED_LIBS=OFF",
        "-DGGML_NATIVE=OFF",
        "-DGGML_OPENMP=OFF",
        "-DLLAMA_CURL=OFF",
        "-DLLAMA_BUILD_TESTS=OFF",
        "-DLLAMA_BUILD_EXAMPLES=OFF",
        "-DLLAMA_BUILD_SERVER=OFF",
        "-DLLAMA_BUILD_TOOLS=OFF"]
    if arch:
        cfg.append("-DGGML_CPU_ARM_ARCH=" + arch)
    run([cmake] + cfg)
    # Only the libraries. Upstream's CLI target links implementation libs that
    # the OFF switches above remove, so asking for it would fail the link --
    # and the library is what we want.
    run([cmake, "--build", lbuild, "--target", "llama", "ggml", "ggml-cpu", "ggml-base"])

    say("[llama] assembling %s" % lsdk)
    inc, libdir = os.path.join(lsdk, "include"), os.path.join(lsdk, "lib")
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
            fail("[llama] expected archive missing: %s" % lib)
    b.copy_all(libs, libdir)

    say("[llama] done -> %s" % lsdk)
    if os.path.normcase(os.path.abspath(lsdk)) != os.path.normcase(default_lsdk):
        say("[llama] that is NOT the auto-detected location, so name it in")
        say("[llama] entry/build-profile.json5:")
        say('[llama]   "arguments": "-DLLAMA_CPP_DIR=%s"' % b.fwd(lsdk))


# -----------------------------------------------------------------------------
# MNN with the omni Talker, cross-compiled for this ABI.
#
# This is an EVALUATION lane, not a dependency: nothing in the app links it,
# and it is deliberately excluded from `all` so a bare build does not spend
# twenty minutes on an engine the core does not use.
#
# It exists to answer one question that cannot be answered on a desktop. MNN
# is the only way to get the model's own voice out -- llama.cpp has no Talker
# checkpoint in GGUF -- and it works: measured, an 8.8 s clip in and 4.0 s of
# real speech out. What is NOT known is whether generating that speech is
# affordable on a phone. On a desktop CPU it runs at about 3x slower than real
# time, which is the figure that decides the feature, and a desktop figure
# does not transfer. See design/voice.md.
#
# Output: prebuilt/mnn-sdk/<abi>/{include,lib}, shaped like the llama SDK so
# that whatever links it later resolves it by repo-relative path.
# -----------------------------------------------------------------------------
def build_mnn(o, sdk):
    src = b.env("MNN_SRC", b.sibling("MNN"))
    if not os.path.isfile(os.path.join(src, "CMakeLists.txt")):
        fail('[mnn] no MNN at "%s".' % src,
             "[mnn] clone it at the pinned commit:",
             "[mnn]   " + b.sync_cmd("clone", "MNN"),
             "[mnn] or set MNN_SRC to an existing checkout.")
    say("[mnn] src=%s" % src)
    b.third_verify("MNN", "MNN_SRC", src)

    _native, toolchain, cmake, ninja = tools(sdk, "mnn")
    mbuild = os.path.join(HERE, ".mnn-build", o.abi)
    msdk = os.path.join(HERE, "prebuilt", "mnn-sdk", o.abi)
    if o.clean:
        say("[mnn] cleaning")
        b.rmtree(mbuild)
        b.rmtree(msdk)

    # MNN_BUILD_LLM_OMNI forces MNN_BUILD_OPENCV and MNN_BUILD_AUDIO on: the
    # omni path needs both front ends compiled in even when only audio is used
    # at run time. Which of them actually LOADS is a runtime flag -- is_visual /
    # is_audio / has_talker are plain booleans in the model's llm_config.json --
    # so the 353 MB vision encoder costs nothing here beyond code size.
    #
    # MNN has its OWN shared-library switch and ignores the standard CMake one:
    # with only BUILD_SHARED_LIBS=OFF it happily builds five .so files.
    # MNN_SEP_BUILD has to go with it -- it splits the backends into separate
    # shared objects and upstream documents it as meaningful only when shared.
    #
    # MNN_SME2 has to be turned off EXPLICITLY -- it defaults ON, and leaving it
    # alone does not leave it off. Two reasons to disable it. It does not build
    # here: its kernels call the fp16 FML intrinsics (vfmlalq_low_f16), which
    # need `+fp16fml`, while MNN compiles those sources with
    # `-march=armv8.2-a+fp16`, and the OHOS clang does not supply the extension
    # by default the way other toolchains apparently do. And it would not pay:
    # SME2 is Armv9, which no phone this app targets has. Like the `+sve` note
    # on DEFAULT_ARCH, it is a feature to measure before enabling, not one to
    # inherit from a default.
    cfg = cross(src, mbuild, toolchain, ninja, o.abi) + [
        "-DBUILD_SHARED_LIBS=OFF",
        "-DMNN_BUILD_SHARED_LIBS=OFF",
        "-DMNN_SEP_BUILD=OFF",
        "-DMNN_BUILD_LLM=ON",
        "-DMNN_BUILD_LLM_OMNI=ON",
        "-DMNN_LOW_MEMORY=ON",
        "-DMNN_SUPPORT_TRANSFORMER_FUSE=ON",
        "-DMNN_BUILD_TOOLS=OFF",
        "-DMNN_BUILD_BENCHMARK=OFF",
        "-DMNN_BUILD_TEST=OFF",
        "-DMNN_SME2=OFF"]
    # MNN_ARM82 turns on the fp16 kernels every current arm64 phone has.
    if o.abi == "arm64-v8a":
        cfg.append("-DMNN_ARM82=ON")
    run([cmake] + cfg)
    run([cmake, "--build", mbuild, "--target", "MNN", "llm"])

    say("[mnn] assembling %s" % msdk)
    inc, libdir = os.path.join(msdk, "include"), os.path.join(msdk, "lib")
    b.mkdirs(inc, libdir)
    # MNN's public headers, plus the llm engine's own. Copied rather than
    # referenced so the SDK is self-contained the way the llama one is.
    b.copy_tree(os.path.join(src, "include", "MNN"), os.path.join(inc, "MNN"))
    b.copy_tree(os.path.join(src, "transformers", "llm", "engine", "include", "llm"),
                os.path.join(inc, "llm"))

    # And MNN's OWN rapidjson, which is not a convenience -- it is the fix for
    # a crash. rapidjson is header-only templates, so its member functions land
    # in the object files of whoever includes it, as weak symbols. The whole
    # app is one .so, the linker keeps exactly ONE definition of each, and this
    # repo already carries a much newer rapidjson under prebuilt/<abi>/include
    # for src/llm and src/utils. MNN's llm engine parses config.json with the
    # 1.1.0 release it vendors.
    #
    # Two layouts, one symbol: the newer MemoryPoolAllocator keeps a
    # SharedData* where 1.1.0 keeps chunkHead_ and userBuffer_. Whichever copy
    # wins the link runs against the other one's objects. Measured: SIGSEGV at
    # 0x10 inside MemoryPoolAllocator::Malloc, on the FIRST json parse of
    # Llm::createLLM, before a single log line of MNN's own.
    #
    # So the SDK ships the version libMNN.a was compiled with, and the root
    # CMakeLists puts it ahead of the prebuilt one. One rapidjson per .so.
    b.copy_tree(os.path.join(src, "3rd_party", "rapidjson"), os.path.join(inc, "rapidjson"))
    if not os.path.isfile(os.path.join(inc, "rapidjson", "document.h")):
        fail("[mnn] no rapidjson under %s" % inc,
             "[mnn] MNN vendors it at 3rd_party/rapidjson -- look there")

    # ONE archive, and that is correct: with MNN_SEP_BUILD=OFF the llm engine is
    # an OBJECT library, so its objects are folded into libMNN.a rather than
    # sitting beside it. There is no libllm.a to find, and looking for one
    # reports a successful build as a failure -- which is how this check first
    # read it. Verified by symbol: Omni, Talker, generateWavform and
    # setWavformCallback are all inside the 14.5 MB archive.
    archive = os.path.join(mbuild, "libMNN.a")
    if not os.path.isfile(archive):
        fail("[mnn] no libMNN.a under %s" % mbuild,
             "[mnn] MNN moves its output around between versions -- look there")
    b.copy_all([archive], libdir)
    say("[mnn] SDK at %s" % msdk)


# -----------------------------------------------------------------------------
# qwentts.cpp: Qwen3-TTS, for the model's own voice reading text.
#
# A THIRD engine, and the bar for that is high -- design/voice.md has the
# arithmetic. What it buys that neither llama.cpp nor MNN can: a voice that
# holds across utterances AND keeps its prosody. llama.cpp runs the same
# checkpoint, but its only reference path pools the speaker into a fixed
# vector (attentive statistics pooling, [1536,T] -> [3072,1]) and the
# time-varying part goes with it; MNN's Talker cannot be driven from text at
# all. Measured on a desktop: VoiceDesign 1.16x real time, in-context cloning
# 1.97x, against 3.0x for MNN's omni Talker.
#
# Narrow, unlike MNN: one purpose, ggml underneath, a C ABI in src/qwen.h
# (qt_init / qt_synthesize / qt_free) and an upstream termux script, so it is
# built for a phone by people other than us.
#
# STATIC, like the other two lanes -- qwen-core is a static library by default
# and QWEN_SHARED stays off. A HAP has nowhere to put a companion .so.
#
# Output: prebuilt/qtts-sdk/<abi>/{include,lib}, shaped like the llama and MNN
# SDKs so the app's CMakeLists finds it the same way.
# -----------------------------------------------------------------------------
def build_qtts(o, sdk):
    src = b.env("QTTS_SRC", b.sibling("qwentts.cpp"))
    if not os.path.isfile(os.path.join(src, "CMakeLists.txt")):
        fail('[qtts] no qwentts.cpp at "%s".' % src,
             "[qtts] clone it at the pinned commit, submodule and patches included:",
             "[qtts]   " + b.sync_cmd("clone", "qwentts.cpp"),
             "[qtts] or set QTTS_SRC to an existing checkout.")
    say("[qtts] src=%s" % src)
    # The ggml submodule is deliberately NOT checked for. This lane stopped
    # compiling it -- see below -- so demanding it would be a precondition on
    # something the build does not read.

    # ONE ggml for both lanes, and llama.cpp's is the one. This lane used to
    # compile qwentts's own ggml submodule, and the app linked that copy for
    # everything because it superset llama's. The 2026-09-21 update inverted
    # that: libllama now wants ggml_dsv4_hc_pre_gated, which the fork does not
    # have, so whichever of the two copies is kept, one symbol goes unmet and
    # the app does not link. design/voice.md has both symbol tables.
    #
    # GGML_SOURCE_DIR, which qwentts added the same day, is the way out: it
    # points the qwentts build at any ggml tree, so both lanes compile the SAME
    # one from the SAME commit and the question stops existing. What it costs
    # is the fork's ggml_compute_forward_snake_fused, a graph pattern match
    # that collapses the DAC decoder's snake activation into one kernel -- same
    # output without it, some unmeasured amount slower, in the stage that is
    # 45% of the synthesis.
    #
    # So this target needs a llama.cpp checkout even when `llama` is not being
    # built, which is the reason for the check.
    llama_src = b.env("LLAMA_SRC", b.sibling("llama.cpp"))
    if not os.path.isfile(os.path.join(llama_src, "ggml", "CMakeLists.txt")):
        fail('[qtts] qtts compiles llama.cpp\'s ggml and there is none at "%s".'
             % os.path.join(llama_src, "ggml"),
             "[qtts]   " + b.sync_cmd("clone", "llama.cpp"),
             "[qtts] or set LLAMA_SRC to an existing checkout.")
    say("[qtts] ggml=%s" % os.path.join(llama_src, "ggml"))

    # Patches, applied to somebody else's checkouts. third/patches/ carries the
    # full reasoning for each; in one line:
    #
    #   ggml-cpu.c   OHOS takes the __linux__ affinity branch, where musl needs
    #                _GNU_SOURCE for cpu_set_t and has no pthread_setaffinity_np.
    #                Applied to LLAMA_SRC, because that is the ggml being
    #                compiled now. qwentts-ggml-ohos-affinity.patch is the same
    #                fix for the submodule, kept in third/patches for whoever
    #                points GGML_SOURCE_DIR back at it, and deliberately not
    #                applied here.
    #   backend.h    the CPU thread count is fixed at hardware_concurrency()/2
    #                with no parameter, and that heuristic is about SMT, not
    #                phones.
    #
    # Applied here rather than committed there because the checkouts are
    # upstream's and get updated.
    b.qpatch(llama_src, "llama-ggml-ohos-affinity.patch")
    b.qpatch(src, "qwentts-thread-count-override.patch")
    b.third_verify("llama.cpp", "LLAMA_SRC", llama_src)
    b.third_verify("qwentts.cpp", "QTTS_SRC", src)

    native, toolchain, cmake, ninja = tools(sdk, "qtts")
    qbuild = os.path.join(HERE, ".qtts-build", o.abi)
    qsdk = os.path.join(HERE, "prebuilt", "qtts-sdk", o.abi)
    if o.clean:
        say("[qtts] cleaning")
        b.rmtree(qbuild)
        b.rmtree(qsdk)

    # The tools are the reason llama-style flags are not enough here: qwen-tts,
    # qwen-codec and tts-server all build by default and all want a main(),
    # which a cross build has no use for. Only qwen-core is asked for by name
    # below.
    #
    # GGML_NATIVE=OFF because -march=native is meaningless cross-compiling and
    # the probe runs on the host. No -D_GNU_SOURCE on the command line: the
    # affinity patch puts the #define in the one file that needs it. Both
    # errors are real and both are fixed there -- cpu_set_t and CPU_ZERO, which
    # musl hides behind _GNU_SOURCE (the OHOS toolchain passes
    # -D_XOPEN_SOURCE=600), and pthread_setaffinity_np, which musl does not
    # have at all and which the patch routes to sched_setaffinity, the same
    # call the Android branch already makes.
    cfg = cross(src, qbuild, toolchain, ninja, o.abi) + [
        "-DGGML_SOURCE_DIR=" + b.fwd(llama_src) + "/ggml",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DQWEN_SHARED=OFF",
        "-DGGML_NATIVE=OFF",
        "-DGGML_OPENMP=OFF"]
    # The same feature set the llama lane uses, and set the same way -- these
    # two are the same ggml compiled twice, so a difference here would be an
    # accident rather than a decision. `arch=` / `baseline` steer both.
    arch = DEFAULT_ARCH if o.arch is None else o.arch
    if o.abi == "arm64-v8a" and arch:
        cfg.append("-DGGML_CPU_ARM_ARCH=" + arch)
    if o.vulkan:
        cfg += vulkan_args(native, ninja)

    say("[qtts] configuring")
    run([cmake] + cfg)
    say("[qtts] building qwen-core")
    run([cmake, "--build", qbuild, "--target", "qwen-core"])

    say("[qtts] assembling %s" % qsdk)
    inc, libdir = os.path.join(qsdk, "include"), os.path.join(qsdk, "lib")
    b.mkdirs(inc, libdir)
    # Only the C ABI header. Everything else under src/ is the implementation,
    # and including any of it would tie this repo to qwentts.cpp's internals.
    b.copy_headers([os.path.join(src, "src", "qwen.h")], inc)

    # Every .a the build produced: qwen-core plus ggml's, which are separate
    # archives (ggml-base, ggml-cpu, ggml). Collected by search rather than by
    # name because ggml splits them differently between versions, and a
    # missing one surfaces as an undefined symbol at the app's link step
    # instead of here.
    count = b.copy_all(b.find(qbuild, "*.a"), libdir)
    if count < 2:
        fail("[qtts] only %d archive(s) under %s -- expected qwen-core plus ggml's" % (count, qbuild))
    say("[qtts] SDK at %s (%d archives)" % (qsdk, count))


def vulkan_args(native, ninja):
    """The GPU, off unless asked for. It needs a Vulkan SDK on the HOST as well
    as the driver on the phone, and the flags exist because the two halves
    come from different places:

      glslc, SPIRV-Headers   host only. 151 compute shaders are compiled at
                             build time by a vulkan-shaders-gen that ggml
                             builds for the host and then runs -- a host build
                             nested inside a cross build.
      Vulkan_INCLUDE_DIR     host, and this is the one that is NOT obvious.
                             The OHOS sysroot ships the C headers and
                             ggml-vulkan includes <vulkan/vulkan.hpp>, the C++
                             bindings, which are not there. Compiling against
                             the host SDK's headers and linking the phone's
                             loader is fine: the loader ABI is stable and ggml
                             queries every extension at runtime anyway.
      Vulkan_LIBRARY         the phone's, from the sysroot.

    FIND_ROOT_PATH_MODE_PACKAGE=BOTH because the OHOS toolchain re-roots
    package searches into the sysroot, so ggml's own `list(APPEND
    CMAKE_PREFIX_PATH $ENV{VULKAN_SDK})` is ignored and SPIRV-Headers is never
    found."""
    vk = b.env("VULKAN_SDK")
    if not vk:
        fail("[qtts] vulkan needs a Vulkan SDK on this machine (VULKAN_SDK is unset).")

    # The SDK lays its host tools out differently per platform: Bin/Include/Lib
    # on Windows, bin/include/lib everywhere else.
    def sub(*names):
        for n in names:
            if os.path.isdir(os.path.join(vk, n)):
                return os.path.join(vk, n)
        return os.path.join(vk, names[0])
    vkbin, vkinc, vklib = sub("Bin", "bin"), sub("Include", "include"), sub("Lib", "lib")
    # And it lays SPIRV-Headers out two ways as well: the upstream layout puts
    # the config in a directory named for the package, which is the only shape
    # find_package looks for, and the LunarG SDK drops it straight into cmake/
    # where nothing finds it. Naming the directory covers both, since this flag
    # is passed either way.
    spirv = os.path.join(vklib, "cmake", "SPIRV-Headers")
    if not os.path.isfile(os.path.join(spirv, "SPIRV-HeadersConfig.cmake")):
        spirv = os.path.join(vklib, "cmake")

    # ninja on PATH because the nested host configure inherits the generator
    # but not CMAKE_MAKE_PROGRAM, and fails with "unable to find a build
    # program corresponding to Ninja" -- which reads like a missing tool rather
    # than an unpropagated variable.
    b.prepend_path(os.path.dirname(ninja))
    say("[qtts] vulkan backend ON, SDK %s" % vk)
    return ["-DGGML_VULKAN=ON",
            "-DVulkan_GLSLC_EXECUTABLE=" + b.fwd(os.path.join(vkbin, "glslc" + b.EXE)),
            "-DVulkan_INCLUDE_DIR=" + b.fwd(vkinc),
            "-DVulkan_LIBRARY=" + b.fwd(native) + "/sysroot/usr/lib/aarch64-linux-ohos/libvulkan.so",
            "-DSPIRV-Headers_DIR=" + b.fwd(spirv),
            "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH"]


# -----------------------------------------------------------------------------
# The app itself: compile and sign the HAP, without opening the IDE.
#
# DevEco is an IntelliJ front end over hvigor, and hvigor is a command-line
# tool -- so the IDE is a convenience, not a requirement. The three things it
# otherwise sets up for you:
#
#   NODE_HOME         hvigor runs on node, and DevEco ships one.
#   DEVECO_SDK_HOME   the SDK *root* (the dir holding default/), NOT the
#                     openharmony dir OHOS_SDK_ROOT names. Getting this wrong
#                     fails with "Invalid value of 'DEVECO_SDK_HOME' in the
#                     system environment path", which does not hint that the
#                     variable was simply unset.
#   ohpm on PATH      for dependency resolution.
#
# Signing comes from the signingConfigs block in build-profile.json5, the same
# one DevEco writes -- so a HAP built here is signed exactly as one built in
# the IDE. Without that block the build still succeeds and produces only the
# unsigned artifact, which is fine for CI and not installable on a device.
# -----------------------------------------------------------------------------
def build_hap(deveco):
    if not deveco:
        fail("[hap] DevEco Studio not found. Set DEVECO_HOME to its install directory.")
    name = "hvigorw.bat" if b.WINDOWS else "hvigorw"
    # The standalone command-line-tools package lays hvigorw out at bin/.
    for hvigorw in (os.path.join(deveco, "tools", "hvigor", "bin", name),
                    os.path.join(deveco, "bin", name)):
        if os.path.isfile(hvigorw):
            break
    else:
        fail('[hap] no hvigorw under "%s" -- is DEVECO_HOME right?' % deveco)

    node = os.path.join(deveco, "tools", "node")
    os.environ["NODE_HOME"] = node
    os.environ["DEVECO_SDK_HOME"] = os.path.join(deveco, "sdk")
    b.prepend_path(os.path.join(deveco, "tools", "ohpm", "bin"))
    b.prepend_path(node)
    if not b.WINDOWS:
        b.prepend_path(os.path.join(node, "bin"))

    say("[hap] hvigor from %s" % deveco)
    try:
        run([hvigorw, "--mode", "module", "-p", "product=default", "assembleHap"], cwd=HERE)
    except b.Stop:
        fail("[hap] build failed")

    out = os.path.join(HERE, "entry", "build", "default", "outputs", "default")
    say("[hap] done -> %s" % out)
    if os.path.isfile(os.path.join(out, "entry-default-signed.hap")):
        say("[hap]   entry-default-signed.hap    -- installable: hdc install <file>")
    if os.path.isfile(os.path.join(out, "entry-default-unsigned.hap")):
        say("[hap]   entry-default-unsigned.hap")


if __name__ == "__main__":
    b.entry(main)
