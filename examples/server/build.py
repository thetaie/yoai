#!/usr/bin/env python3
"""Usage: python build.py [clean]

The speech server: yoai-server, the core behind a WebSocket, built into
build/ here.

  (no args)  configure if needed, then build
  clean      delete build/ first
  help       this text; also -h or --help

Speech output is always compiled in: the server has nothing else to serve.
Its SDK is the root build's, build/qtts-sdk at the repository root, built here
the same way `tts` builds it there: qwentts.cpp and llama.cpp checkouts,
fetched into third/ the first time, and the GPU through CUDA or Vulkan when
there is one. libwebsockets comes from vcpkg, through vcpkg.json's `server`
feature.

Settings are the root build's, from .env at the repository root -- see
.env.example: VS_DIR, NINJA, VCPKG_ROOT, LLAMA_SRC, QTTS_SRC, VULKAN_SDK,
CUDA_PATH, TTS_BACKEND.
"""

# Python 3.8 or newer, standard library only.

import importlib.util
import os
import sys

# What every build script shares is in the root build.py. Loaded by path, under
# a name of its own: `import build` from here would import this file.
sys.dont_write_bytecode = True  # no __pycache__ in the repository
_spec = importlib.util.spec_from_file_location(
    "yoai_build", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build.py"))
b = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(b)
say, fail, run = b.say, b.fail, b.run

SERVER = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(SERVER, "build")
WORDS = ("clean",)


def main(argv):
    want = set()
    for tok in argv:
        t = tok.lower()
        if t in ("help", "-h", "--help"):
            say(__doc__.strip())
            return 0
        if t not in WORDS:
            fail('Unknown argument "%s". Run %s --help for the options.' % (tok, b.script_cmd(__file__)))
        want.add(t)

    # After the arguments, so the help text still prints when a configured path
    # is wrong. The names are the paths this script reads.
    if b.WINDOWS:
        names = ("VS_DIR", "NINJA", "VCPKG_ROOT", "LLAMA_SRC", "QTTS_SRC", "VULKAN_SDK", "CUDA_PATH")
    else:
        names = ("VCPKG_ROOT", "LLAMA_SRC", "QTTS_SRC", "VULKAN_SDK", "CUDA_PATH")
    if not b.load_env(*names):
        return 1

    if "clean" in want:
        b.rmtree(BUILD)

    gen = []
    if b.WINDOWS:
        gen = ["-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + b.visual_studio()]

    # The checkouts the SDK compiles, fetched when they are not there. A tree
    # moved past its pin is built anyway, with the warning qtts_sdk() gives.
    b.fetch_checkouts()
    sdk = b.qtts_sdk(gen)
    args = ["-DCMAKE_BUILD_TYPE=Release", "-DQTTS_DIR=" + b.fwd(sdk)] + vcpkg_args()

    # The core finds each GPU backend's archive in the SDK by name, so a tree
    # configured before one appeared in the SDK, or after it went, is
    # configured again: it would link libggml's reference to that backend
    # against nothing, or name an archive that is gone. CUDA's toolkit goes
    # along, the one that compiled the archive, for the runtime libraries it
    # needs.
    libdir = os.path.join(sdk, "lib")
    with_cuda = b.has(libdir, b.lib("ggml-cuda"))
    if with_cuda:
        args.append("-DCUDAToolkit_ROOT=" + b.fwd(os.path.dirname(os.path.dirname(b.nvcc()))))
    stale = any(b.cached_lib(BUILD, var) != b.has(libdir, b.lib(name))
                for var, name in (("GGML_CUDA_LIB", "ggml-cuda"), ("GGML_VULKAN_LIB", "ggml-vulkan")))

    if not b.configured(BUILD) or stale:
        run(["cmake", "-S", SERVER, "-B", BUILD] + gen + args)
    run(["cmake", "--build", BUILD, "--config", "Release", "--parallel"])
    say("[build] %s" % b.shown(os.path.join(BUILD, "yoai-server" + b.EXE)))
    return 0


def vcpkg_args():
    """The vcpkg toolchain, from the tree the root build uses, with the
    manifest's `server` feature for libwebsockets.

    VCPKG_MANIFEST_DIR because the source directory is this one, and the
    toolchain looks for vcpkg.json only in the source directory. The install
    goes into build/vcpkg_installed here, not the root's vcpkg_installed/: a
    manifest install removes what its features do not ask for, so sharing
    that directory with builds that lack `server` would take libwebsockets out
    from under this one. vcpkg's binary cache makes the second copy of curl
    and OpenSSL a restore, not a build."""
    toolchain = os.path.join(b.vcpkg_tree(), "scripts", "buildsystems", "vcpkg.cmake")
    args = ["-DCMAKE_TOOLCHAIN_FILE=" + toolchain]
    if b.WINDOWS:
        args.append("-DVCPKG_TARGET_TRIPLET=x64-windows")
    return args + ["-DVCPKG_INSTALLED_DIR=" + os.path.join(BUILD, "vcpkg_installed"),
                   "-DVCPKG_MANIFEST_DIR=" + b.ROOT,
                   "-DVCPKG_MANIFEST_FEATURES=server"]


if __name__ == "__main__":
    b.entry(main)
