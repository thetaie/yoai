#!/usr/bin/env python3
"""Usage: python build.py [clean] [asr] [llama] [tts]

The Python package: the core as a shared library, with the ctypes wrapper in
yoai/, as a wheel that this machine can pip install.

  (no args)  build the root build/ with the lanes it has, then the wheel into
             dist/. A fresh build/ gets the lanes the root build picks by
             itself
  clean, asr, llama, tts
             passed on to the root build.py, whose --help says what each
             does; then the wheel
  help       this text; also -h or --help

The library is the root build's build/yoai.dll, libyoai.so or libyoai.dylib,
with the lanes that build has. The wheel is for this machine only: it is
tagged with this platform, and carries what the library loads from build/
-- on Windows, curl, OpenSSL and zlib -- but not what this machine already
has, such as the Visual C++ runtime, the Vulkan loader or the CUDA runtime.

Install it into any Python 3.8 or newer here:
  pip install --force-reinstall dist/yoai-<version>-py3-none-<platform>.whl

Settings are the root build's, from .env at the repository root.
"""

# A wheel is a zip with three metadata files, so it is written here with
# zipfile rather than with setuptools or pip: no package to install before the
# first build, and nothing compiled per Python version -- the wrapper is
# ctypes, which is why one wheel serves every Python on this machine.
#
# Python 3.8 or newer, standard library only.

import base64
import hashlib
import importlib.util
import os
import platform
import re
import sys
import zipfile

# What every build script shares is in the root build.py. Loaded by path, under
# a name of its own: `import build` from here would import this file.
sys.dont_write_bytecode = True  # no __pycache__ in the repository
_spec = importlib.util.spec_from_file_location(
    "yoai_build", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build.py"))
b = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(b)
say, fail, run = b.say, b.fail, b.run

PYTHON = os.path.dirname(os.path.abspath(__file__))
PACKAGE = os.path.join(PYTHON, "yoai")
DIST = os.path.join(PYTHON, "dist")
BUILD = os.path.join(b.ROOT, "build")
WORDS = ("clean", "asr", "llama", "tts")

if b.WINDOWS:
    LIBRARY = "yoai.dll"
elif b.MACOS:
    LIBRARY = "libyoai.dylib"
else:
    LIBRARY = "libyoai.so"


def main(argv):
    lanes = []
    for tok in argv:
        t = tok.lower()
        if t in ("help", "-h", "--help"):
            say(__doc__.strip())
            return 0
        if t not in WORDS:
            fail('Unknown argument "%s". Run %s --help for the options.' % (tok, b.script_cmd(__file__)))
        lanes.append(t)

    # The root build does the compiling, with everything it reads from .env.
    run([sys.executable, os.path.join(b.ROOT, "build.py")] + lanes)

    library = os.path.join(BUILD, LIBRARY)
    if not os.path.isfile(library):
        fail("[wheel] no %s in %s. Was build/ configured with YOAI_BUILD_SHARED=OFF?"
             % (LIBRARY, b.shown(BUILD)))
    files = [("yoai/" + name, os.path.join(PACKAGE, name))
             for name in sorted(os.listdir(PACKAGE)) if name.endswith(".py")]
    files.append(("yoai/" + LIBRARY, library))
    if b.WINDOWS:
        files += [("yoai/" + os.path.basename(p), p) for p in beside(library)]

    wheel = write_wheel(files)
    say("[wheel] %s" % b.shown(wheel))
    say("[wheel]   pip install --force-reinstall %s" % b.shown(wheel))
    return 0


def beside(library):
    """The DLLs `library` loads, directly or through each other, that sit next
    to it in build/: the ones vcpkg's toolchain copied there. Anything else it
    imports comes from Windows or from what this machine has installed.
    Found by reading the imports, as examples/desktop/build.py does, so the
    list follows the dependencies by itself."""
    b.visual_studio()  # for dumpbin
    here = os.path.dirname(library)
    present = {n.lower(): os.path.join(here, n) for n in os.listdir(here) if n.lower().endswith(".dll")}
    found, todo = {}, [library]
    while todo:
        text = b.output(["dumpbin", "/nologo", "/dependents", todo.pop()])
        if text is None:
            fail("[wheel] dumpbin could not read the imports of %s" % b.shown(library))
        for line in text.splitlines():
            key = line.strip().lower()
            if key in present and key not in found:
                found[key] = present[key]
                todo.append(present[key])
    return sorted(found.values())


def core_version():
    """The version yoai_version() returns, read from its definition, so the
    wheel and the library inside it cannot disagree."""
    m = re.search(r'yoai_version\(void\)\s*\{\s*return\s*"([^"]+)"',
                  b.read(os.path.join(b.ROOT, "src", "yoai.cpp")))
    if not m:
        fail("[wheel] could not find the version in src/yoai.cpp")
    return m.group(1)


def platform_tag():
    """This machine's platform, as a wheel names it. pip installs a wheel only
    where its tag is one the running Python supports, so the tag is also what
    keeps it off a Python of another architecture."""
    machine = platform.machine().lower()
    if b.WINDOWS:
        tags = {"amd64": "win_amd64", "x86_64": "win_amd64", "arm64": "win_arm64"}
        if machine not in tags:
            fail("[wheel] no wheel tag for Windows on %s" % platform.machine())
        return tags[machine]
    if b.MACOS:
        # The macOS this was built on: the library is built for it, not for
        # anything older. Since macOS 11 a tag carries the major version only.
        ver = platform.mac_ver()[0].split(".")
        release = ver[0] + "_0" if int(ver[0]) >= 11 else "_".join((ver + ["0"])[:2])
        return "macosx_%s_%s" % (release, machine)
    return "linux_" + machine


def write_wheel(files):
    """dist/yoai-<version>-py3-none-<platform>.whl from `files`, a list of
    (path in the wheel, path on disk), plus the metadata pip reads."""
    version = core_version()
    tag = "py3-none-" + platform_tag()
    info = "yoai-%s.dist-info" % version
    metadata = ("Metadata-Version: 2.1\n"
                "Name: yoai\n"
                "Version: %s\n"
                "Summary: The YoAI core through ctypes\n"
                "License: MIT\n"
                "Requires-Python: >=3.8\n" % version)
    wheel_info = ("Wheel-Version: 1.0\n"
                  "Generator: examples/python/build.py\n"
                  "Root-Is-Purelib: false\n"
                  "Tag: %s\n" % tag)

    b.mkdirs(DIST)
    path = os.path.join(DIST, "yoai-%s-%s.whl" % (version, tag))
    # Written beside it and renamed, so a build that stops halfway leaves no
    # wheel that pip would take for a whole one.
    part = path + ".part"
    record = []
    with zipfile.ZipFile(part, "w", zipfile.ZIP_DEFLATED) as z:
        def write(name, data):
            # A fixed time, so the same inputs give the same wheel, and
            # rw-r--r--: zipfile's default is rw-------, which pip keeps.
            entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            entry.external_attr = 0o644 << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(entry, data)

        def add(name, data):
            write(name, data)
            digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=")
            record.append("%s,sha256=%s,%d" % (name, digest.decode("ascii"), len(data)))

        for name, src in files:
            with open(src, "rb") as f:
                add(name, f.read())
        add(info + "/METADATA", metadata.encode("utf-8"))
        add(info + "/WHEEL", wheel_info.encode("utf-8"))
        # RECORD lists every file with its hash, and itself without one.
        record.append(info + "/RECORD,,")
        write(info + "/RECORD", ("\n".join(record) + "\n").encode("utf-8"))
    os.replace(part, path)
    return path


if __name__ == "__main__":
    b.entry(main)
