#!/usr/bin/env python3
"""Usage: python build.py [clean] [native] [llama] [qtts] [mnn] [app] [dist]

Desktop client build: the Node-API addon, the Electron bundles, and the app
as a directory that runs on its own.

  (no args)  native + app + dist, with llama and qtts. With no lane named, the llama.cpp and qwentts.cpp checkouts are fetched
             into third/ when they are not there (git and the network, once),
             and each lane is built when its checkouts are on what
             third/manifest.json pins. mnn only when named.
  native     just the C++ side: the Node-API addon built from native/
             (build/yoai.node). Rebuild it after any change under src/ or
             native/, with the app closed -- a running app holds the file.
  llama      assemble a desktop llama.cpp SDK and build the addon WITH the
             on-device lane. Implies native. Recognition rides along: it
             links ggml and never libllama, so the same SDK turns it on.
  qtts       assemble a desktop qwentts.cpp SDK and build the addon with
             speech output, on the GPU where there is one -- Metal on macOS,
             Vulkan elsewhere -- and on the CPU, with a warning, where there
             is none. Implies native. Needs a llama.cpp checkout too, whether
             or not `llama` is asked for: it compiles llama.cpp's ggml.
  mnn        assemble a desktop MNN SDK and build the addon with the omni
             lane: audio in, a spoken answer out, no text in between.
             Implies native. A second inference engine and 3.8 GB of
             weights -- see design/voice.md before turning it on.
  app        just the Electron bundles (out/)
  dist       native + app, then the app as one directory that runs where it
             is put: dist-<os>-<arch>/ -- YoAI.exe in dist-win-x64/, YoAI.app
             in dist-mac-arm64/, yoai in dist-linux-x64/. For the machine it
             runs on only; there is no cross packaging. Lanes as for a bare
             build: llama and qtts unless others are named. A few seconds on
             top of the rest; close a running YoAI first.
  clean      discard build/ first. ADDING a lane does not need it -- the
             addon is reconfigured every run, so `llama` on an existing tree
             takes effect by itself. DROPPING one does: a lane that is not
             asked for is not passed as OFF, it is not passed at all, and
             CMake keeps the cached value. So `native` on a tree built with
             `llama qtts` rebuilds it with both still on. The SDKs under sdk/
             are NOT discarded either way.
  help       this text; also -h or --help

Settings, each read from .env at the repository root -- see .env.example --
or the environment, .env winning; YOAI_<NAME> in the environment wins over
both, for one run:
  VS_DIR            Windows: Visual Studio install root
  NINJA             Windows: ninja.exe (default: the one Visual Studio bundles)
  VCPKG_ROOT        vcpkg checkout, for curl and OpenSSL (default:
                    ../../third/vcpkg, cloned on the first run at vcpkg.json's
                    builtin-baseline)
  LLAMA_SRC         llama.cpp checkout (default: ../../third/llama.cpp).
                    Read by `llama` AND by `qtts`, which compiles its ggml.
  QTTS_SRC          qwentts.cpp checkout (default: in ../../third), `qtts` only
  MNN_SRC           MNN checkout (default: in ../../third), `mnn` only
  VULKAN_SDK        Vulkan SDK, for `qtts` off macOS
"""

# The addon is the slow half and changes rarely; the bundles are seconds. They
# are separate arguments for that reason -- the loop while writing UI is
# `npm run dev`, and nothing here has to run again until the C++ changes.
#
# Python 3.8 or newer, standard library only.

import glob
import importlib.util
import os
import platform
import shutil
import sys

# What every build script shares is in the root build.py. Loaded by path, under
# a name of its own: `import build` from here would import this file.
sys.dont_write_bytecode = True  # no __pycache__ in the repository
_spec = importlib.util.spec_from_file_location(
    "yoai_build", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build.py"))
b = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(b)
say, fail, run = b.say, b.fail, b.run

DESKTOP = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(DESKTOP, "build")
# The SDKs live OUTSIDE build/, and that is not tidiness. build/ belongs to
# cmake-js: when its build step fails it runs its own CLEAN, which deletes the
# whole output directory. With the SDKs inside, one compile error in
# binding.cpp costs a full llama.cpp rebuild and a full 151-shader Vulkan
# rebuild -- minutes, for a typo. `clean` still only discards build/, so the
# SDKs survive a lane change too, which is the other thing that used to
# rebuild them for nothing.
SDK = os.path.join(DESKTOP, "sdk")
# What `dist` puts in the package's resources/, laid out as it will be there.
STAGE = os.path.join(DESKTOP, "stage")
WORDS = ("clean", "native", "llama", "qtts", "mnn", "app", "dist")
LANES = {"llama", "qtts", "mnn"}
# What an addon build with no lane named adds by itself, and the checkouts
# each needs. mnn wants asking for: a second engine and 3.8 GB of weights.
DEFAULT = (("llama", ("llama.cpp",)), ("qtts", ("qwentts.cpp", "llama.cpp")))


def main(argv):
    want = set()
    for tok in argv:
        t = tok.lower()
        if t in ("help", "-h", "--help"):
            say(__doc__.strip())
            return 0
        # The old name for native, named here so it is not mistaken for a typo.
        if t == "addon":
            fail("`addon` is now `native`: %s native" % b.script_cmd(__file__))
        if t not in WORDS:
            fail('Unknown argument "%s". Run %s --help for the options.' % (tok, b.script_cmd(__file__)))
        want.add(t)
    if "dist" in want:
        want |= {"native", "app"}
    if want & LANES:
        want.add("native")
    # No target named means all of them, which is what a first run wants: an
    # app that runs. Naming native or app is the quick way round while
    # working, and leaves dist out.
    if not want & {"native", "app"}:
        want |= {"native", "app", "dist"}

    # After the arguments, so the help text still prints when a configured path
    # is wrong. The names are the paths this script reads.
    if b.WINDOWS:
        names = ("VS_DIR", "NINJA", "VCPKG_ROOT", "LLAMA_SRC", "QTTS_SRC", "MNN_SRC", "VULKAN_SDK")
    else:
        names = ("VCPKG_ROOT", "LLAMA_SRC", "QTTS_SRC", "MNN_SRC", "VULKAN_SDK")
    if not b.load_env(*names):
        return 1

    if "clean" in want:
        b.rmtree(BUILD)

    if not os.path.isdir(os.path.join(DESKTOP, "node_modules")):
        say("[build] npm install")
        run(["npm", "install"], cwd=DESKTOP)

    if "native" in want:
        if not want & LANES:
            want |= b.pick_lanes(DEFAULT, __file__, "the addon", "mnn")
        native(want)
    if "app" in want:
        say("[build] electron-vite")
        run(["npm", "run", "build"], cwd=DESKTOP)
    if "dist" in want:
        package()
    return 0


# -----------------------------------------------------------------------------
# The Node-API addon
# -----------------------------------------------------------------------------
def native(want):
    # The generator for the SDK trees: Ninja from Visual Studio on Windows,
    # whatever CMake picks everywhere else.
    gen = []
    if b.WINDOWS:
        ninja = b.visual_studio()
        gen = ["-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + ninja]

    # The addon is loaded by Electron, not by Node, so it is built against
    # Electron's headers. Read from the installed package rather than pinned
    # here: a version that disagrees with the one in node_modules is a module
    # that refuses to load, and the error names an ABI rather than a version.
    electron = b.output(["node", "-p", "require('electron/package.json').version"], cwd=DESKTOP)
    if not electron:
        fail("Could not read electron's version. Run `npm install` in examples/desktop/ first.")
    say("[build] addon against electron %s" % electron)

    args = vcpkg_args()
    if "llama" in want:
        args += ["--CDYOAI_ONDEVICE_LLM=ON", "--CDLLAMA_CPP_DIR=" + llama_sdk()]
    if "qtts" in want:
        args += ["--CDYOAI_QTTS=ON", "--CDQTTS_DIR=" + qtts_sdk(gen)]
    if "mnn" in want:
        args += ["--CDYOAI_MNN=ON", "--CDMNN_DIR=" + mnn_sdk(gen)]
        # --CDYOAI_MNN_VULKAN=ON stays off, with MNN_VULKAN under mnn_sdk and
        # for the same reason. The two travel together: this one says the
        # Vulkan symbols exist to link against, and referencing them in a
        # libMNN built without the backend does not link at all.

    # Two things here are worked around rather than configured, and both cost
    # an afternoon to find a second time:
    #
    #  - `node <entry>` and NOT `npx cmake-js`. npx prepends node_modules/.bin
    #    to PATH for everything it spawns, and one of cmake-js's own
    #    dependencies installs a shim there called `rc`. On Windows CMake then
    #    picks that up as the RESOURCE COMPILER instead of the Windows SDK's
    #    rc.exe, and the failure is "RC Pass 1 ... unknown error" while probing
    #    whether cl.exe works at all.
    #
    #  - Windows: `--generator Ninja` and not cmake-js's own choice. Its Visual
    #    Studio detection finds VS 2026 and then reports "There is no Visual C++
    #    compiler installed", because it does not know that generator's name.
    #    visual_studio() has already put cl.exe on PATH, so Ninja needs nothing
    #    from it. CMAKE_BUILD_TYPE has to be named with a single-config
    #    generator, and it must be Release: vcpkg's x64-windows libraries are
    #    /MD, and a Debug addon would be /MDd against them.
    #
    # configure AND build, the same list to both, because `compile` on its own
    # drops every --CD flag on a build directory that already has a cache.
    # cmake-js configures only when there is none, so a lane switched on by an
    # argument to this script stays OFF in the cache -- and NOTHING SAYS SO.
    # The addon still links, still exports the same entry points (see
    # binding.cpp on the tts half) and only answers nativeLocalAvailable() with
    # false at runtime, which reads as a missing model rather than an ignored
    # flag. CMake is idempotent and quick when nothing changed, and `build`
    # then does the incremental work.
    common = ["--directory", os.path.join(DESKTOP, "native"),
              "--out", BUILD,
              "--runtime", "electron",
              "--runtime-version", electron]
    if b.WINDOWS:
        common += ["--generator", "Ninja", "--CDCMAKE_MAKE_PROGRAM=" + ninja]
    common += ["--CDCMAKE_BUILD_TYPE=Release"] + args
    cmake_js = os.path.join(DESKTOP, "node_modules", "cmake-js", "bin", "cmake-js")
    for step in ("configure", "build"):
        run(["node", cmake_js, step] + common)

    addon = addon_path()
    if not addon:
        fail("[build] cmake-js reported success but there is no yoai.node under %s" % BUILD)
    say("[build] addon at %s" % addon)


def addon_path():
    """The addon this tree built, or None. A single-config generator writes it
    at the root of the output directory, a multi-config one under Release/.
    Both are checked because the module loader in src/main/core.ts searches
    both, and the build's message is the one place that can say which one
    actually happened."""
    for addon in (os.path.join(BUILD, "yoai.node"), os.path.join(BUILD, "Release", "yoai.node")):
        if os.path.isfile(addon):
            return addon
    return None


def vcpkg_args():
    """The vcpkg toolchain for the addon, on every platform, from the tree
    the root build uses -- the root build's policy, vcpkg_args() there.

    VCPKG_MANIFEST_DIR because the source directory cmake-js configures is
    native/, and the vcpkg toolchain looks for vcpkg.json only in the source
    directory. Without it the toolchain runs in classic mode, installs
    nothing, and finds curl and OpenSSL only because the root build happened
    to install them into the shared vcpkg_installed/ first -- on a fresh clone,
    where nothing has, find_package(CURL) would fail. MANIFEST_MODE is passed
    as well because it is a cached option."""
    toolchain = os.path.join(b.vcpkg_tree(), "scripts", "buildsystems", "vcpkg.cmake")

    # It does NOT rescue a tree first configured without the manifest: vcpkg
    # refuses to switch an existing build directory to manifest mode, and
    # records the mode it started in as Z_VCPKG_CHECK_MANIFEST_MODE. Such a
    # tree has to start over, which is said here rather than in vcpkg's own
    # words -- those ask for the directory to be deleted and do not mention the
    # lanes it had.
    if "Z_VCPKG_CHECK_MANIFEST_MODE:INTERNAL=OFF" in b.read(os.path.join(BUILD, "CMakeCache.txt")):
        fail("[build] build/ was configured before the addon used the root vcpkg.json,",
             "[build] and vcpkg cannot switch an existing tree to manifest mode. Start it",
             "[build] over, naming the lanes it had -- `clean` does not keep them:",
             "[build]   %s clean native [llama] [qtts] [mnn]" % b.script_cmd(__file__))

    args = ["--CDCMAKE_TOOLCHAIN_FILE=" + toolchain]
    if b.WINDOWS:
        args.append("--CDVCPKG_TARGET_TRIPLET=x64-windows")
    return args + ["--CDVCPKG_INSTALLED_DIR=" + os.path.join(b.ROOT, "vcpkg_installed"),
                   "--CDVCPKG_MANIFEST_DIR=" + b.ROOT,
                   "--CDVCPKG_MANIFEST_MODE=ON"]


# The C runtime, on Windows, and it is not a preference. cmake-js forces /MT on
# the addon unconditionally (cMake.js: "MD builds will crash upon attempting to
# free memory"), CMake's own default is /MD, and MSVC records the choice in
# every .obj. Mixing them is LNK2038 once per translation unit plus a page of
# unresolved __imp_* -- which is what a link against an unflagged SDK looks
# like, and it says nothing about the CRT in the message.
STATIC_CRT = ["-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"] if b.WINDOWS else []

# Position-independent code, off Windows, and for the same kind of reason: the
# addon is a shared object, so every static archive linked into it must be
# built -fPIC. ggml and llama turn it on for themselves; qwentts.cpp does not,
# and the link then fails on a thread_local in qwen.cpp (R_X86_64_TPOFF32
# against __tls_guard). Passed to every SDK so none depends on its own default.
PIC = [] if b.WINDOWS else ["-DCMAKE_POSITION_INDEPENDENT_CODE=ON"]


# -----------------------------------------------------------------------------
# A desktop llama.cpp SDK, shaped like the ones examples/harmony/build.py assembles per
# ABI: include/ plus lib/. Unlike the root build's `asr`, this one needs
# libllama as well as ggml, so it is the long build -- minutes, not one.
# -----------------------------------------------------------------------------
def llama_sdk():
    src = b.env("LLAMA_SRC", b.third_dir("llama.cpp"))
    if not os.path.isfile(os.path.join(src, "ggml", "include", "ggml.h")):
        fail("no llama.cpp at %s." % src,
             "clone it at the pinned commit, with its patch applied:",
             "  " + b.sync_cmd("clone", "llama.cpp"),
             "or point LLAMA_SRC at an existing checkout.")
    b.third_verify("llama.cpp", "LLAMA_SRC", src)

    lbuild = os.path.join(SDK, "llama")
    sdk = os.path.join(SDK, "llama-sdk")
    say("[build] llama.cpp from %s" % src)

    # Configured every run, not only when the tree is new. The guard that used
    # to be here saved a few seconds and cost a whole class of bug: a flag added
    # below has no effect on a tree already configured without it, so the build
    # keeps succeeding and the addon keeps failing to link. CMake is idempotent
    # and quick when nothing changed. No generator named, as before: this tree
    # keeps whichever one it was first configured with.
    b.fresh_tree(lbuild, src)
    run(["cmake", "-S", src, "-B", lbuild,
         "-DCMAKE_BUILD_TYPE=Release"] + STATIC_CRT + PIC + b.QUIET_THIRD + [
         "-DBUILD_SHARED_LIBS=OFF",
         "-DLLAMA_BUILD_TESTS=OFF",
         "-DLLAMA_BUILD_EXAMPLES=OFF",
         "-DLLAMA_BUILD_TOOLS=OFF",
         "-DLLAMA_BUILD_SERVER=OFF",
         "-DLLAMA_CURL=OFF"])
    # --parallel only off Windows: with no generator named, CMake can pick NMake
    # there, which warns that it cannot build in parallel.
    run(["cmake", "--build", lbuild, "--config", "Release"] + ([] if b.WINDOWS else ["--parallel"]) +
        ["--target", "llama", "ggml", "ggml-cpu", "ggml-base"])

    inc, libdir = os.path.join(sdk, "include"), os.path.join(sdk, "lib")
    b.mkdirs(inc, libdir)
    b.copy_headers_glob(os.path.join(src, "ggml", "include", "*.h"), inc)
    b.copy_headers([os.path.join(src, "include", "llama.h")], inc)
    b.copy_all(b.find(lbuild, *[b.lib(t) for t in ("llama", "ggml", "ggml-cpu", "ggml-base")]), libdir)
    if not b.has(libdir, b.lib("llama")):
        fail("[build] llama built but nothing was copied into %s" % libdir)
    say("[build] llama SDK at %s" % sdk)
    return sdk


# -----------------------------------------------------------------------------
# A desktop qwentts.cpp SDK, shaped like the llama one: include/ plus lib/.
#
# WITH THE PLATFORM'S GPU BACKEND, which is the whole point of building it
# here. On this laptop's CPU the lane measures 1.13x real time and on its
# integrated Arc 0.63x, and below 1 is a threshold rather than a slope: a
# stream needs a pre-buffer of duration x (RTF - 1), which at 0.63 is nothing
# and at 1.13 is eight seconds for a minute of audio.
#
#   macOS    Metal, and ggml turns it on ITSELF -- ggml/CMakeLists.txt does
#            `if (APPLE) set(GGML_METAL_DEFAULT ON)`. So nothing is passed,
#            and passing GGML_VULKAN would be wrong: there is no native Vulkan
#            on macOS.
#   Windows  Vulkan, from a Vulkan SDK (VULKAN_SDK) for glslc and the C++
#   Linux    headers ggml-vulkan includes. On Linux the distro's loader,
#            headers and glslc do as well.
#
# Far fewer flags than examples/harmony/build.py's cross build needs. Two of the four
# obstacles there were artefacts of cross-compiling -- a nested host build
# inheriting the generator but not CMAKE_MAKE_PROGRAM, and a sysroot carrying
# Vulkan's C headers but not the C++ ones. On the host, find_package(Vulkan)
# just works. SPIRV-Headers does NOT, and that one is not about cross
# compiling at all -- see spirv_args() in the root build.py.
#
# One thing does carry over: ggml builds vulkan-shaders-gen for the host and
# runs it over 151 shaders, and MSVC fails that nested configure with "Cannot
# open compiler generated file" when the path gets long. sdk/qtts here is
# short enough; a build directory nested much deeper is not.
# -----------------------------------------------------------------------------
def qtts_sdk(gen):
    src = b.env("QTTS_SRC", b.third_dir("qwentts.cpp"))
    if not os.path.isfile(os.path.join(src, "src", "qwen.h")):
        fail("no qwentts.cpp at %s." % src,
             "clone it at the pinned commit, submodule and patches included:",
             "  " + b.sync_cmd("clone", "qwentts.cpp"),
             "or point QTTS_SRC at an existing checkout.")
    b.third_verify("qwentts.cpp", "QTTS_SRC", src)

    # ONE ggml for both lanes, and llama.cpp's is the one. qwentts.cpp builds a
    # ggml of its own from a fork, and until 2026-09-21 that copy superset
    # llama.cpp's, so linking qwentts's satisfied both libraries. The update
    # inverted it -- libllama now wants ggml_dsv4_hc_pre_gated, which the fork
    # does not have -- and whichever copy is kept, one symbol goes unmet.
    #
    # Linking llama's copy instead is not an option HERE specifically: only
    # the qtts configure asks for GGML_VULKAN, so llama's SDK has no
    # ggml-vulkan library, and the GPU is the entire reason this lane exists.
    # GGML_SOURCE_DIR, which qwentts added the same day, is the way out:
    # qwentts compiles llama.cpp's ggml, with Vulkan, and the one archive set
    # satisfies qwen-core's 134 symbols and libllama's 288 both.
    #
    # What it costs is real and unmeasured: the fork carries
    # ggml_compute_forward_snake_fused, a graph pattern match in ggml-cpu.c
    # that collapses the DAC decoder's snake activation into one kernel.
    # llama's ggml has no such fusion, so those five ops run unfused -- same
    # output, some unknown amount slower, in the codec stage that is already
    # 45% of the bill. design/voice.md has the symbol tables.
    ggml = b.env("LLAMA_SRC", b.third_dir("llama.cpp"))
    if not os.path.isfile(os.path.join(ggml, "ggml", "CMakeLists.txt")):
        fail("qtts compiles llama.cpp's ggml, and there is none at %s." % os.path.join(ggml, "ggml"),
             "  " + b.sync_cmd("clone", "llama.cpp"))
    b.third_verify("llama.cpp", "LLAMA_SRC", ggml)

    # Without a GPU backend the lane builds for the CPU and says so, as the
    # root build's does: it works, only slower than real time, and a first
    # build that stops for want of a Vulkan SDK is worse than one that warns.
    # GGML_VULKAN is passed either way off macOS, and the tree is configured
    # every run, so the backend follows the machine rather than the first run.
    vulkan_sdk = b.env("VULKAN_SDK")
    vulkan = False
    backend = []
    if b.MACOS:
        say("[build] qtts backend: Metal (ggml enables it for APPLE by itself)")
    elif vulkan_sdk or (not b.WINDOWS and b.on_path("glslc")):
        vulkan = True
        backend = ["-DGGML_VULKAN=ON"] + b.spirv_args(vulkan_sdk)
        say("[build] qtts backend: Vulkan" + (", from " + vulkan_sdk if vulkan_sdk else ""))
    else:
        backend = ["-DGGML_VULKAN=OFF"]
        b.warn("[build] warning: no Vulkan found, so qtts is built for the CPU only.")
        b.warn("[build]   It works, slower: RTF 1.13 against 0.63 on the GPU where measured.")
        if b.WINDOWS:
            b.warn("[build]   For the GPU, install the LunarG Vulkan SDK, which sets VULKAN_SDK,")
            b.warn("[build]   or set VULKAN_SDK in .env; then run this again.")
        else:
            b.warn("[build]   For the GPU: apt install libvulkan-dev glslc (Debian/Ubuntu),")
            b.warn("[build]   dnf install vulkan-loader-devel glslc (Fedora), or set VULKAN_SDK")
            b.warn("[build]   in .env; then run this again.")

    qbuild = os.path.join(SDK, "qtts")
    sdk = os.path.join(SDK, "qtts-sdk")
    say("[build] qwentts.cpp from %s, ggml from %s" % (src, os.path.join(ggml, "ggml")))

    # CMAKE_POLICY_DEFAULT_CMP0091=NEW is what makes STATIC_CRT do anything
    # here. qwentts.cpp declares cmake_minimum_required(VERSION 3.14), so
    # CMP0091 is OLD for its whole tree, and under OLD the runtime library is
    # not an abstraction at all -- /MD is a literal string inside
    # CMAKE_CXX_FLAGS_RELEASE and CMAKE_MSVC_RUNTIME_LIBRARY is read by nobody.
    # llama.cpp asks for a newer CMake, so it obeyed the flag and qwentts
    # silently did not; the link error then named only the three ggml
    # archives, which look like the common half. The same 3.14 is why
    # QUIET_THIRD matters most here; see it in the root build.py.
    #
    # Configured every run, for the reason given under llama_sdk().
    crt = ["-DCMAKE_POLICY_DEFAULT_CMP0091=NEW"] + STATIC_CRT if b.WINDOWS else []
    b.fresh_tree(qbuild, src)
    run(["cmake", "-S", src, "-B", qbuild] + gen + [
        "-DCMAKE_BUILD_TYPE=Release"] + crt + PIC + b.QUIET_THIRD + [
        "-DGGML_SOURCE_DIR=" + b.fwd(ggml) + "/ggml",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DQWEN_SHARED=OFF"] + backend)

    # Re-running cmake cannot take /MD back out of a cache that already has it:
    # CMAKE_CXX_FLAGS_RELEASE is initialised once and then kept. So the flags
    # above only work on a tree configured WITH them, and a tree configured
    # without them keeps building happily and failing at the addon's link. Said
    # here, where the cache is, rather than 300 lines of LNK2038 later.
    if b.WINDOWS:
        for line in b.read(os.path.join(qbuild, "CMakeCache.txt")).splitlines():
            if line.startswith("CMAKE_CXX_FLAGS_RELEASE:STRING") and "/MD" in line:
                fail("[build] the qtts tree at %s is configured for the /MD runtime," % qbuild,
                     "[build] which cannot link into cmake-js's /MT addon. Delete it and rebuild.")

    # Only qwen-core by name: qwen-tts, qwen-codec and tts-server all build by
    # default and all want a main(), which the addon has no use for.
    run(["cmake", "--build", qbuild, "--config", "Release", "--parallel", "--target", "qwen-core"])

    # lib/ is laid out from empty every run, and a tree once built with Vulkan
    # keeps that archive after the backend is turned off: the core links
    # whatever this directory holds, so it is taken out. include/ is kept, so
    # that copy_headers() can tell which headers changed.
    inc, libdir = os.path.join(sdk, "include"), os.path.join(sdk, "lib")
    b.rmtree(libdir)
    b.mkdirs(inc, libdir)
    b.copy_headers_glob(os.path.join(src, "src", "qwen*.h"), inc)
    # Collected by search rather than by name because ggml splits its archives
    # differently between versions, and the GPU backend adds one more; a
    # missing one surfaces as an undefined symbol at the addon's link step.
    patterns = ["*.lib"] if b.WINDOWS else ["libqwen-core.*", "libggml*.*"]
    count = b.copy_all(b.find(qbuild, *patterns), libdir)
    if not vulkan:
        for p in b.find(libdir, b.lib("ggml-vulkan")):
            os.remove(p)
            count -= 1
    if not b.has(libdir, b.lib("qwen-core")):
        fail("[build] qwentts built but no qwen-core library is in %s" % libdir)
    if vulkan and not b.has(libdir, b.lib("ggml-vulkan")):
        fail("[build] qwentts built WITHOUT the Vulkan backend -- check that cmake found it")
    say("[build] qtts SDK at %s (%d libraries)" % (sdk, count))
    return sdk


# -----------------------------------------------------------------------------
# A desktop MNN SDK, shaped like the other two: include/ plus lib/.
#
# The SECOND inference engine, and the only reason to carry one: MNN has the
# omni Talker -- thinker to codec tokens to a diffusion transformer to a
# vocoder -- and llama.cpp has no Talker checkpoint in GGUF at all. This is the
# end-to-end lane: audio in, a spoken answer out, with no text in the middle.
#
# MNN_WIN_RUNTIME_MT is the CRT on Windows, and it is not optional. MNN sets
# CMP0091 NEW itself and then picks MultiThreadedDLL unless this is on;
# cmake-js forces /MT on the addon, and the mismatch is the same page of
# LNK2038 the qtts lane taught. Unlike qwentts, MNN has its own switch for it,
# so no policy override is needed here.
#
# MNN_SEP_BUILD=OFF goes with MNN_BUILD_SHARED_LIBS=OFF: MNN has its own
# shared-library switch and ignores the standard one, and with the two off the
# llm engine becomes an OBJECT library folded into libMNN rather than a second
# archive. There is no llm library to find, and looking for one reports a good
# build as a failure.
#
# MNN_VULKAN is DELIBERATELY ABSENT, and the reason is a measurement rather
# than an omission. Turning it on needs three things together: -DMNN_VULKAN=ON,
# -DMNN_VULKAN_IMAGE=OFF (the default image path is the classic CNN backend --
# no attention, no layernorm, no low-bit weights, so the DiT cannot run on it),
# and YOAI_MNN_VULKAN in native(), because a static libMNN drops every one of
# the backend's registrars unless the archive is linked whole. All three are in
# place; see CMakeLists.txt and third/patches.
#
# The idea was sound and the first half of it held. The model's config.json has
# two runtimes -- the main one carries the thinker and the Talker, the `mllm`
# block carries the audio encoder, the diffusion transformer and the vocoder --
# so moving only `mllm` to the GPU should overlap them properly. Today they run
# on separate threads and the same four cores, which is why the split adds up
# to the wall clock instead of hiding behind it. With `mllm` on Vulkan the
# Talker's decode did drop from 3.57 s to 1.91 s, exactly as predicted.
#
# What it produced was noise. Vulkan returned -4, VK_ERROR_DEVICE_LOST, several
# times per chunk: the GPU work never completed, so token2wav "finished" in
# 0.87 s against 3.07 s on the CPU and the buffers it left behind were garbage.
# The whole exchange was 7.5 s against 6.5 s.
#
# So it is off until there is a machine, or an MNN, where that does not happen.
#
# Measured on Windows only; the flags carry nothing Windows-specific but
# MNN_WIN_RUNTIME_MT.
# -----------------------------------------------------------------------------
def mnn_sdk(gen):
    src = b.env("MNN_SRC", b.third_dir("MNN"))
    if not os.path.isfile(os.path.join(src, "CMakeLists.txt")):
        fail("no MNN at %s." % src,
             "clone it at the pinned commit:",
             "  " + b.sync_cmd("clone", "MNN"),
             "or point MNN_SRC at an existing checkout.")
    b.third_verify("MNN", "MNN_SRC", src)

    mbuild = os.path.join(SDK, "mnn")
    sdk = os.path.join(SDK, "mnn-sdk")
    say("[build] MNN from %s" % src)

    # Configured every run, for the reason given under llama_sdk().
    b.fresh_tree(mbuild, src)
    run(["cmake", "-S", src, "-B", mbuild] + gen + [
        "-DCMAKE_BUILD_TYPE=Release"] + PIC + b.QUIET_THIRD +
        (["-DMNN_WIN_RUNTIME_MT=ON"] if b.WINDOWS else []) + [
        "-DBUILD_SHARED_LIBS=OFF",
        "-DMNN_BUILD_SHARED_LIBS=OFF",
        "-DMNN_SEP_BUILD=OFF",
        "-DMNN_BUILD_LLM=ON",
        "-DMNN_BUILD_LLM_OMNI=ON",
        "-DMNN_LOW_MEMORY=ON",
        "-DMNN_SUPPORT_TRANSFORMER_FUSE=ON",
        "-DMNN_BUILD_TOOLS=OFF",
        "-DMNN_BUILD_BENCHMARK=OFF",
        "-DMNN_BUILD_TEST=OFF"])
    run(["cmake", "--build", mbuild, "--config", "Release", "--parallel", "--target", "MNN", "llm"])

    say("[build] assembling %s" % sdk)
    inc, libdir = os.path.join(sdk, "include"), os.path.join(sdk, "lib")
    b.mkdirs(inc, libdir)
    b.copy_tree(os.path.join(src, "include", "MNN"), os.path.join(inc, "MNN"))
    b.copy_tree(os.path.join(src, "transformers", "llm", "engine", "include", "llm"),
                os.path.join(inc, "llm"))

    # And MNN's OWN rapidjson, which is not a convenience -- it is the fix for
    # a crash. rapidjson is header-only templates, so its member functions land
    # in the object files of whoever includes it, as weak symbols; the whole
    # addon is one module and the linker keeps exactly ONE definition of each.
    # This repo already carries a much newer rapidjson for src/llm and
    # src/utils, and libMNN was compiled against the 1.1.0 release it vendors.
    # Two layouts, one symbol: measured as a SIGSEGV at 0x10 inside
    # MemoryPoolAllocator::Malloc, on the FIRST json parse of Llm::createLLM,
    # before a line of MNN's own logging.
    b.copy_tree(os.path.join(src, "3rd_party", "rapidjson"), os.path.join(inc, "rapidjson"))
    if not os.path.isfile(os.path.join(inc, "rapidjson", "document.h")):
        fail("[build] no rapidjson under %s" % inc,
             "[build] MNN vendors it at 3rd_party/rapidjson -- look there")

    b.copy_all(b.find(mbuild, b.lib("MNN")), libdir)
    if not b.has(libdir, b.lib("MNN")):
        fail("[build] MNN built but no MNN library is in %s" % libdir)
    say("[build] MNN SDK at %s" % sdk)
    return sdk


# -----------------------------------------------------------------------------
# The package: electron-builder, with electron-builder.yml, over out/ and what
# is staged here.
#
# stage/ is the package's resources/ directory, laid out as it will be there:
# yoai.node, every library it loads that a fresh machine may not have, and the
# speaker voice files.
#
#   Windows  Node loads a .node with the search path altered to its own
#            directory, so the DLLs beside it are the ones found -- before any
#            copy in System32, which a machine with the Visual C++
#            redistributable installed has and a fresh one does not.
#   Linux    the addon carries RUNPATH $ORIGIN (native/CMakeLists.txt), which
#            does the same for the .so files beside it.
#   macOS    nothing is bundled: every SDK is static, and what the addon loads
#            is the system's. Checked, not assumed -- see check_runtime_macos().
# -----------------------------------------------------------------------------
def package():
    addon = addon_path()
    if not addon:
        fail("[dist] no yoai.node under %s" % BUILD)
    os_name = "win" if b.WINDOWS else "mac" if b.MACOS else "linux"
    arch = host_arch()
    b.rmtree(STAGE)
    b.mkdirs(STAGE)
    shutil.copy2(addon, STAGE)
    if b.WINDOWS:
        # curl, OpenSSL and zlib: vcpkg's x64-windows triplet is dynamic, and
        # its toolchain has already copied them beside the addon.
        b.copy_glob(os.path.join(os.path.dirname(addon), "*.dll"), STAGE)
        bundle_runtime()
    elif b.MACOS:
        check_runtime_macos()
    else:
        bundle_runtime_linux()

    # Every speaker's voice files; src/main/reader.ts picks one by name.
    for spk in sorted(glob.glob(os.path.join(b.ROOT, "speaker", "*", "*.spk"))):
        dest = os.path.join(STAGE, "speaker", os.path.basename(os.path.dirname(spk)))
        b.mkdirs(dest)
        shutil.copy2(spk, dest)

    # electron-builder writes into a scratch directory, stage-out/ (see
    # electron-builder.yml); the app is moved out of it to dist-<os>-<arch>/
    # and the rest, its own debug notes, goes. Both from empty, so nothing of
    # an earlier package is left in either. `node <cli>` rather than npx, for
    # the reason given under native().
    scratch = os.path.join(DESKTOP, "stage-out")
    app_dir = os.path.join(DESKTOP, "dist-%s-%s" % (os_name, arch))
    b.rmtree(scratch)
    # Moved aside before it is deleted: a running YoAI.exe holds its files,
    # and a delete would stop halfway through, leaving half an app. Windows
    # refuses to move a directory with a running program in it, whole.
    if os.path.isdir(app_dir):
        old = app_dir + ".old"
        b.rmtree(old)
        try:
            os.rename(app_dir, old)
        except OSError:
            fail("[dist] %s is in use -- close YoAI and run this again." % b.shown(app_dir))
        b.rmtree(old)
    say("[dist] electron-builder")
    cli = os.path.join(DESKTOP, "node_modules", "electron-builder", "cli.js")
    run(["node", cli, "--" + os_name, "--" + arch, "--config", "electron-builder.yml"], cwd=DESKTOP)
    # One directory, named by electron-builder after the platform and the
    # arch: win-unpacked, linux-unpacked, linux-arm64-unpacked, mac-arm64 (with
    # YoAI.app inside), and so on. Found rather than spelled out here.
    built = [d for d in os.listdir(scratch) if os.path.isdir(os.path.join(scratch, d))]
    if len(built) != 1:
        fail("[dist] expected one app directory in %s, found: %s"
             % (b.shown(scratch), ", ".join(built) or "none"))
    os.rename(os.path.join(scratch, built[0]), app_dir)
    b.rmtree(scratch)
    b.rmtree(STAGE)  # copied into the app's resources/, and nothing else reads it
    say("[dist] the app, everything it runs with, in %s" % b.shown(app_dir))


def host_arch():
    """electron-builder's name for this machine's architecture. The package is
    always for the machine it is built on: the addon and the Electron runtime
    npm installed are both this one's."""
    m = platform.machine().lower()
    if m in ("x86_64", "amd64"):
        return "x64"
    if m in ("arm64", "aarch64"):
        return "arm64"
    fail("[dist] no package for this architecture: %s" % platform.machine())


def bundle_runtime():
    """Copy into stage/ the DLLs that what is staged imports and a fresh
    Windows may not have, and stop the build on an import nothing will supply.

    Found by reading the imports, not by naming DLLs here, so a dependency that
    changes changes the list by itself. Today that is:

      vcruntime140.dll  the vcpkg libraries are built against it
      vcomp140.dll      ggml's OpenMP
                        -- both from Visual Studio's redistributable
                        directory, whose files are licensed for shipping
                        beside an application;
      vulkan-1.dll      the Vulkan loader, which a GPU driver installs into
                        System32 and a machine without one lacks. Without it
                        the addon does not load at all, where with it and no
                        driver it finds no device and reads on the CPU. Taken
                        from beside electron.exe, which ships the same
                        Khronos loader: that copy is not found from here, as
                        Node loads a .node with the search path altered to
                        its own directory, which leaves the exe's out.

    An import counts as supplied otherwise only when it is part of the
    Universal CRT (api-ms-win-*, in Windows since 10) or in System32. Anything
    else would be "The specified module could not be found" on a machine that
    did not build this, so it is said here instead."""
    if not b.env("VCToolsRedistDir"):
        b.visual_studio()  # dumpbin, and where the redistributable files are
    redist = {}
    for d in sorted(glob.glob(os.path.join(b.env("VCToolsRedistDir"), "x64", "Microsoft.VC*.*"))):
        for f in glob.glob(os.path.join(d, "*.dll")):
            redist.setdefault(os.path.basename(f).lower(), f)
    if "vcruntime140.dll" not in redist:
        fail('[dist] no Visual C++ redistributable under "%s"' % b.env("VCToolsRedistDir"),
             "[dist] Install the MSVC build tools' redistributable component.")
    electron = os.path.join(DESKTOP, "node_modules", "electron", "dist")
    for f in glob.glob(os.path.join(electron, "*.dll")):
        redist.setdefault(os.path.basename(f).lower(), f)
    system32 = os.path.join(b.env("SystemRoot", r"C:\Windows"), "System32")

    staged = {n.lower() for n in os.listdir(STAGE)}
    todo = sorted(os.path.join(STAGE, n) for n in staged)
    missing = {}
    while todo:
        path = todo.pop()
        for name in pe_imports(path):
            key = name.lower()
            if key in staged or key.startswith(("api-ms-win-", "ext-ms-win-")):
                continue
            # Before System32, which has the redistributable ones and the
            # Vulkan loader here and may not on the machine this is for.
            if key in redist:
                shutil.copy2(redist[key], STAGE)
                staged.add(key)
                todo.append(os.path.join(STAGE, os.path.basename(redist[key])))
                say("[dist] bundling %s, which %s imports" % (os.path.basename(redist[key]),
                                                              os.path.basename(path)))
            elif not os.path.isfile(os.path.join(system32, name)):
                missing.setdefault(name, os.path.basename(path))
    if missing:
        fail("[dist] imported, and nothing in the package or in Windows supplies it:",
             *["[dist]   %s, by %s" % (n, by) for n, by in sorted(missing.items())])


def pe_imports(path):
    """The DLL names `path` imports, as dumpbin lists them."""
    text = b.output(["dumpbin", "/nologo", "/dependents", path])
    if text is None:
        fail('[dist] dumpbin could not read "%s"' % path)
    return [s for s in (line.strip() for line in text.splitlines())
            if s.lower().endswith(".dll") and " " not in s]


# glibc and the GCC runtime. Every desktop distro has them, and a copy of
# either would be wrong: glibc's pieces must match each other and the loader,
# and Electron itself already needs libstdc++ and libgcc_s from the system.
LINUX_SYSTEM = ("linux-vdso.so", "ld-linux", "libc.so", "libm.so", "libdl.so",
                "libpthread.so", "librt.so", "libstdc++.so", "libgcc_s.so")


def bundle_runtime_linux():
    """Copy into stage/ the shared libraries yoai.node loads that a fresh
    Linux may not have, and stop the build on one nothing will supply.

    Found with ldd, not named here, so a dependency that changes changes the
    list by itself. Today that is:

      libgomp.so.1    ggml's OpenMP, from GCC; on most desktops, not all
      libvulkan.so.1  the Vulkan loader, when qtts is built with Vulkan.
                      Taken from Electron's own directory, which ships the
                      same Khronos loader, as on Windows.

    The addon's RUNPATH is $ORIGIN, so these copies are found before the
    system's. What the package does not cover is glibc itself: the addon needs
    at least the glibc of the machine that built it."""
    electron = {}
    for f in glob.glob(os.path.join(DESKTOP, "node_modules", "electron", "dist", "*.so*")):
        electron[os.path.basename(f)] = f
    missing = {}
    for path in sorted(glob.glob(os.path.join(STAGE, "*.node"))):
        text = b.output(["ldd", path])
        if text is None:
            fail('[dist] ldd could not read "%s"' % path)
        # "libgomp.so.1 => /lib/x86_64-linux-gnu/libgomp.so.1 (0x...)",
        # "libfoo.so.1 => not found", or a bare path for the loader.
        for line in text.splitlines():
            parts = line.split()
            if not parts:
                continue
            name = os.path.basename(parts[0])
            if name.startswith(LINUX_SYSTEM) or os.path.isfile(os.path.join(STAGE, name)):
                continue
            found = parts[2] if len(parts) > 2 and parts[1] == "=>" and parts[2].startswith("/") else None
            src = electron.get(name) or found
            if src is None:
                missing.setdefault(name, os.path.basename(path))
                continue
            # copy2 follows the symlink, and the copy keeps the soname.
            shutil.copy2(src, os.path.join(STAGE, name))
            say("[dist] bundling %s, which %s loads" % (name, os.path.basename(path)))
    if missing:
        fail("[dist] loaded, and nothing on this machine supplies it:",
             *["[dist]   %s, by %s" % (n, by) for n, by in sorted(missing.items())])


def check_runtime_macos():
    """Stop the build if yoai.node loads anything a fresh Mac does not have.

    Nothing is bundled on macOS: vcpkg's triplet and every SDK are static, and
    Metal, Accelerate and libc++ are the system's. The one way that goes wrong
    is an optional dependency that a CMake find_package() picked up from
    Homebrew -- libomp for ggml's OpenMP is the likely one -- which links fine
    here and does not load anywhere else. Said here instead."""
    foreign = []
    for path in sorted(glob.glob(os.path.join(STAGE, "*.node"))):
        text = b.output(["otool", "-L", path])
        if text is None:
            fail('[dist] otool could not read "%s"' % path)
        # The first line is the file itself; each after it is "<path> (...)".
        for line in text.splitlines()[1:]:
            dep = line.strip().split(" (")[0]
            if dep and not dep.startswith(("/usr/lib/", "/System/Library/")):
                foreign.append("[dist]   %s, by %s" % (dep, os.path.basename(path)))
    if foreign:
        fail("[dist] loaded, and not part of macOS -- a fresh Mac would not load the addon:",
             *foreign)


if __name__ == "__main__":
    b.entry(main)
