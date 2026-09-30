#!/usr/bin/env python3
"""Usage: python build.py [clean] [asr] [llama] [tts] [server] [cosyvoice]

Builds the core library, the desktop tools in cli/ and the tests in tests/,
into build/: configures if needed, then builds. This is not the desktop app
-- that is desktop/build.py.

  asr        also compile speech recognition in. Needs a llama.cpp checkout
             for ggml; see LLAMA_SRC
  llama      also compile the on-device engine in: chat and embedding on a
             GGUF, for cli/yoai-llama. The same checkout, and a longer build
  tts        also compile speech output (Qwen3-TTS) in. Needs qwentts.cpp
             and llama.cpp checkouts; uses the GPU when there is one --
             Vulkan, and CUDA too where the CUDA Toolkit is installed
  server     also build the WebSocket speech server, src/server/ and
             cli/yoai-server. Installs libwebsockets; speaks only with tts
  cosyvoice  also build CosyVoice3's own CLI into build/cosyvoice/bin, for
             listening; linked into nothing. Needs cosyvoice.cpp and llama.cpp
             checkouts, and downloads ICU and ONNX Runtime on its first run
  clean      delete build/ first
  help       this text; also -h or --help

They combine, e.g. python build.py clean asr tts

Environment, each also as YOAI_<NAME> in the environment or in .env, which
wins -- see .env.example:
  VS_DIR            Windows: Visual Studio install root
  NINJA             Windows: ninja.exe (default: the one Visual Studio bundles)
  VCPKG_ROOT        vcpkg checkout. Windows default: the one Visual Studio
                    bundles. Elsewhere ../vcpkg, used only if it exists
  OPENSSL_ROOT_DIR  macOS/Linux: where to find OpenSSL, when the default
                    search misses it
  LLAMA_SRC         llama.cpp checkout for asr, llama and tts (default: ../llama.cpp)
  QTTS_SRC          qwentts.cpp checkout for tts (default: ../qwentts.cpp)
  COSYVOICE_SRC     cosyvoice.cpp checkout for cosyvoice (default: ../cosyvoice.cpp)
  VULKAN_SDK        Vulkan SDK, for tts on the GPU; optional
  CUDA_PATH         CUDA Toolkit, for tts on an NVIDIA GPU; optional. Without
                    it, nvcc on PATH, then /usr/local/cuda off Windows
"""

# Two things in one file. The bottom half is the root build. The top half is
# what every build script shares -- desktop/, android/ and harmony/ each have a
# build.py of their own -- plus the .env loader third/sync.py reads its
# settings with. Those load this file BY PATH, under the name yoai_build: a
# plain `import build` from android/build.py would import android/build.py.
#
# The rule every helper here follows: a command that fails ends the build with
# that command's exit code and no message of our own, because the tool has
# already said why. A check of our own that fails says what to do instead.
#
# Python 3.8 or newer, standard library only.

import filecmp
import fnmatch
import glob
import json
import os
import re
import shutil
import stat
import subprocess
import sys

sys.dont_write_bytecode = True  # no __pycache__ in the repository

ROOT = os.path.dirname(os.path.abspath(__file__))
# Checkouts live beside the repository, not inside it -- third/sync.py has why.
PARENT = os.path.dirname(ROOT)
SYNC = os.path.join(ROOT, "third", "sync.py")
PATCHES = os.path.join(ROOT, "third", "patches")

WINDOWS = os.name == "nt"
MACOS = sys.platform == "darwin"
EXE = ".exe" if WINDOWS else ""
# The command to type, in the hints printed to the user. Debian and Ubuntu have
# no `python` unless python-is-python3 is installed.
PY = "python" if WINDOWS else "python3"


# =============================================================================
# Shared by every build script and third/sync.py
# =============================================================================

# --- Settings ------------------------------------------------------------------
# Every setting is named YOAI_<NAME> and fills the <NAME> the scripts read.
# Highest wins:
#
#   1. YOAI_<NAME> in the environment
#   2. YOAI_<NAME> in .env at the repository root
#   3. <NAME> in the environment, e.g. the VULKAN_SDK its installer sets
#   4. the calling script's own default
#
# Only YOAI_ lines are taken from .env: KEY=VALUE per line, no spaces around
# `=`, # for comments, and a value may be wrapped in double quotes. Nothing in
# it is expanded or executed. A new setting goes in NAMES and in .env.example.

NAMES = (
    "VS_DIR", "NINJA", "VCPKG_ROOT", "OPENSSL_ROOT_DIR", "VULKAN_SDK", "CUDA_PATH",
    "LLAMA_SRC", "LLAMA_SDK_DIR", "QTTS_SRC", "COSYVOICE_SRC", "MNN_SRC", "DEVECO_HOME",
    "OHOS_SDK_ROOT",
)

_LINE = re.compile(r"^(YOAI_[A-Z0-9_]+)=(.*)$")


def load_env(*required):
    """Fill os.environ by the rules above. `required` are the paths the caller
    READS: each one that is set must exist, or it is named on stderr and this
    returns False. A name left unset is the caller's to default."""
    env_file = os.path.join(ROOT, ".env")
    if os.path.isfile(env_file):
        with open(env_file, encoding="utf-8-sig") as f:
            for line in f:
                m = _LINE.match(line.rstrip("\r\n"))
                if not m:
                    continue
                key, val = m.group(1), m.group(2)
                if len(val) >= 2 and val[0] == '"' and val[-1] == '"':
                    val = val[1:-1]
                if key not in os.environ:
                    os.environ[key] = val

    for name in NAMES:
        val = os.environ.get("YOAI_" + name)
        if val:
            os.environ[name] = val

    ok = True
    for name in required:
        val = os.environ.get(name)
        if val and not os.path.exists(val):
            print('[env] %s is "%s", which does not exist. Fix YOAI_%s in .env, '
                  "or %s / YOAI_%s in the environment." % (name, val, name, name, name),
                  file=sys.stderr)
            ok = False
    return ok


def env(name, default=""):
    """A setting, with an empty value read as unset."""
    return os.environ.get(name) or default


def sibling(name):
    """The default location of a checkout: beside the repository."""
    return os.path.join(PARENT, name)


# --- Output and exit -------------------------------------------------------------
class Stop(Exception):
    """Ends the build with `code`. Whatever went wrong has been said already."""

    def __init__(self, code=1):
        Exception.__init__(self, code)
        self.code = code


def say(*args):
    # Flushed, because the tools this runs write to the same console from their
    # own processes and would otherwise overtake what is still buffered here.
    print(*args, flush=True)


def warn(*args):
    print(*args, file=sys.stderr, flush=True)


def fail(*lines, code=1):
    for line in lines:
        warn(line)
    raise Stop(code)


def shown(path):
    """`path` as someone in the current directory would type it."""
    try:
        return os.path.relpath(path)
    except ValueError:  # on another drive
        return path


def script_cmd(script):
    """How to run `script` again from here, for the hints."""
    return "%s %s" % (PY, shown(script))


def sync_cmd(*args):
    return " ".join((PY, shown(SYNC)) + args)


def entry(fn):
    """Run a script's main(argv) and exit with what it returns."""
    try:
        sys.stdout.reconfigure(errors="replace")
        sys.stderr.reconfigure(errors="replace")
    except AttributeError:
        pass
    try:
        code = fn(sys.argv[1:]) or 0
    except Stop as e:
        code = e.code
    except KeyboardInterrupt:
        code = 130
    sys.exit(code)


# --- Commands ------------------------------------------------------------------
def _resolve(args):
    args = list(args)
    # A bare name goes through PATH and PATHEXT here rather than in the OS:
    # CreateProcess tries only `.exe`, so npm, which is npm.cmd, would not be
    # found at all.
    if not os.path.dirname(args[0]):
        args[0] = shutil.which(args[0]) or args[0]
    return args


def run(args, cwd=None):
    """Run a command. If it fails, the build ends with its exit code."""
    try:
        r = subprocess.run(_resolve(args), cwd=cwd)
    except OSError as e:
        fail("[build] could not run %s: %s" % (args[0], e))
    if r.returncode != 0:
        # A negative code is a signal on POSIX, and sys.exit would print it.
        raise Stop(r.returncode if r.returncode > 0 else 1)


def output(args, cwd=None):
    """A command's stdout, stripped, or None if it failed or is not there."""
    try:
        r = subprocess.run(_resolve(args), cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, universal_newlines=True)
    except OSError:
        return None
    return r.stdout.strip() if r.returncode == 0 else None


def succeeds(args, cwd=None):
    try:
        return subprocess.run(_resolve(args), cwd=cwd, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
    except OSError:
        return False


def which(name):
    return shutil.which(name)


def on_path(name):
    return shutil.which(name) is not None


def prepend_path(directory):
    os.environ["PATH"] = directory + os.pathsep + os.environ.get("PATH", "")


# --- Files ---------------------------------------------------------------------
def _writable(func, path, _exc):
    # Git marks its object files read-only, and on Windows that stops a delete.
    os.chmod(path, stat.S_IWRITE)
    func(path)


def rmtree(path):
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path, onerror=_writable)


def mkdirs(*paths):
    for p in paths:
        os.makedirs(p, exist_ok=True)


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def fwd(path):
    """Forward slashes. CMake reads a backslash in some -D values as an escape."""
    return path.replace("\\", "/")


def configured(build_dir):
    """Has CMake generated a build here, with either generator this repo uses?"""
    return (os.path.isfile(os.path.join(build_dir, "build.ninja"))
            or os.path.isfile(os.path.join(build_dir, "Makefile")))


def cached_lib(build_dir, var):
    """Did the last configure of `build_dir` find the library its find_library
    stored in `var`? A miss is cached too, as <var>-NOTFOUND."""
    m = re.search(r"^%s:FILEPATH=(.*)$" % re.escape(var),
                  read(os.path.join(build_dir, "CMakeCache.txt")), re.M)
    return bool(m) and not m.group(1).endswith("-NOTFOUND")


def find(root, *patterns):
    """Every file under `root` whose name matches one of `patterns`."""
    found = []
    for d, _dirs, files in os.walk(root):
        for f in sorted(files):
            if any(fnmatch.fnmatch(f, p) for p in patterns):
                found.append(os.path.join(d, f))
    return found


def copy_all(paths, dest):
    for p in paths:
        shutil.copy2(p, dest)
    return len(paths)


def copy_glob(pattern, dest):
    return copy_all(sorted(glob.glob(pattern)), dest)


def copy_headers(paths, dest):
    """Copy headers into an SDK's include/, only those whose content changed,
    each stamped with the time of the copy.

    Not copy_all: copy2 keeps the source's modification time, and a header a
    patch changed can still be older than the objects built against the copy
    it replaces -- so ninja rebuilt nothing that includes it. That kept
    qwen3_tts.cpp compiled against a qwen.h from before
    qwentts-streaming-text.patch. Skipping identical files keeps a rebuild
    from recompiling what did not change."""
    for p in paths:
        target = os.path.join(dest, os.path.basename(p))
        if os.path.isfile(target) and filecmp.cmp(p, target, shallow=False):
            continue
        shutil.copyfile(p, target)
    return len(paths)


def copy_headers_glob(pattern, dest):
    return copy_headers(sorted(glob.glob(pattern)), dest)


def copy_tree(src, dest):
    """Copy `src` into `dest`, merging with what is already there."""
    shutil.copytree(src, dest, dirs_exist_ok=True)


def has(directory, pattern):
    return bool(glob.glob(os.path.join(directory, pattern)))


def lib(stem):
    """A static library's file name, as a pattern: MSVC's name, or the archive
    and shared-object names everywhere else."""
    return stem + ".lib" if WINDOWS else "lib" + stem + ".*"


# --- Toolchains ----------------------------------------------------------------
def visual_studio():
    """Windows: take on the environment vcvarsall.bat sets up, as `call
    vcvarsall.bat` did for the batch scripts, and return the ninja to build
    with. Reads VS_DIR and NINJA."""
    vs_dir = env("VS_DIR", os.path.join(env("ProgramFiles", r"C:\Program Files"),
                                        "Microsoft Visual Studio", "18", "Community"))
    ninja = env("NINJA", os.path.join(vs_dir, "Common7", "IDE", "CommonExtensions",
                                      "Microsoft", "CMake", "Ninja", "ninja.exe"))
    bat = os.path.join(vs_dir, "VC", "Auxiliary", "Build", "vcvarsall.bat")

    # vcvarsall runs vswhere.exe by bare name, and the Visual Studio Installer
    # directory that holds it is not on PATH outside a Developer prompt; the
    # miss prints "'vswhere.exe' is not recognized" and vcvarsall carries on.
    child = dict(os.environ)
    installer = os.path.join(env("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                             "Microsoft Visual Studio", "Installer")
    if os.path.isfile(os.path.join(installer, "vswhere.exe")):
        child["PATH"] = installer + os.pathsep + child.get("PATH", "")

    # `set` after vcvarsall, in the same cmd, prints the environment it left
    # behind. /u makes cmd write that as UTF-16, so a path in any language
    # survives the pipe; /d skips any AutoRun the user has configured.
    try:
        r = subprocess.run('cmd /d /u /s /c ""%s" amd64 >nul && set"' % bat,
                           stdout=subprocess.PIPE, env=child)
    except OSError:
        r = None
    if r is None or r.returncode != 0:
        fail('Could not run vcvarsall.bat under "%s". Set VS_DIR.' % vs_dir)

    # vcvarsall overwrites VCPKG_ROOT with the copy bundled with Visual Studio;
    # the user's own value, when they set one, is kept.
    user_vcpkg = env("VCPKG_ROOT")
    for line in r.stdout.decode("utf-16-le", errors="replace").splitlines():
        key, eq, val = line.partition("=")
        # cmd's per-drive entries look like `=C:=C:\...` and are not variables.
        if eq and key:
            os.environ[key] = val
    if user_vcpkg:
        os.environ["VCPKG_ROOT"] = user_vcpkg
    return ninja


def spirv_args(vulkan_sdk):
    """SPIRV-Headers named explicitly, for a host build against a Vulkan SDK.

    ggml's own `list(APPEND CMAKE_PREFIX_PATH $ENV{VULKAN_SDK})` does not find
    it, and the failure reads like a missing SDK rather than a layout
    difference. find_package's config search looks under
    <prefix>/lib/cmake/<name>*/ -- a directory NAMED for the package -- and
    the LunarG SDK puts SPIRV-HeadersConfig.cmake straight into lib/cmake with
    no such directory. Both layouts exist in the wild, so both are probed, and
    neither present leaves the flag off: ggml's own error names the package
    and is better than a flag pointing somewhere empty. Distro packages use
    the upstream layout and need no help."""
    if not vulkan_sdk:
        return []
    for sub in ("Lib", "lib"):
        for d in (os.path.join(vulkan_sdk, sub, "cmake", "SPIRV-Headers"),
                  os.path.join(vulkan_sdk, sub, "cmake")):
            if os.path.isfile(os.path.join(d, "SPIRV-HeadersConfig.cmake")):
                return ["-DSPIRV-Headers_DIR=" + fwd(d)]
    return []


def nvcc():
    """The CUDA Toolkit's compiler, or None when there is no toolkit.

    CUDA_PATH first, the variable the Windows installer sets; then PATH; then
    /usr/local/cuda, where the Linux installer puts it without adding it to
    PATH. A CUDA_PATH with no compiler in it is a broken install, and saying
    so beats a CPU build that looks like a choice."""
    exe = "nvcc" + EXE
    cuda = env("CUDA_PATH")
    if cuda:
        path = os.path.join(cuda, "bin", exe)
        if not os.path.isfile(path):
            fail('[build] CUDA_PATH is "%s", which has no bin/%s.' % (cuda, exe),
                 "[build] Repair the CUDA Toolkit install, or set YOAI_CUDA_PATH in .env.")
        return path
    found = which("nvcc")
    if found:
        return found
    if not WINDOWS and os.path.isfile("/usr/local/cuda/bin/nvcc"):
        return "/usr/local/cuda/bin/nvcc"
    return None


def cmake_at_least(major, minor):
    """Is the cmake on PATH at least `major`.`minor`?"""
    m = re.search(r"(\d+)\.(\d+)", output(["cmake", "--version"]) or "")
    return bool(m) and (int(m.group(1)), int(m.group(2))) >= (major, minor)


# --- Checkouts -----------------------------------------------------------------
_verified = set()


def third_verify(name, var, tree):
    """Is this checkout what third/manifest.json records? `name` the manifest
    section, `var` its variable there, `tree` the tree about to be compiled.

    A WARNING, never a stop: a tree ahead of its pin is a normal state while
    trying something (third/README.md), and a missing patch matters to some
    lanes and not others. The tree goes over as YOAI_<var>, the one name
    sync.py's own loader will not override, so the tree checked is the tree
    built. Once per name per run."""
    if name in _verified:
        return
    _verified.add(name)
    child = dict(os.environ)
    child["YOAI_" + var] = tree
    if subprocess.run([sys.executable, SYNC, "verify", name], env=child).returncode != 0:
        warn("[build] warning: %s differs from third/manifest.json (above) -- building anyway. "
             "Details: %s" % (name, sync_cmd("check", name)))


def qpatch(tree, name):
    """Apply one patch from third/patches/ to a checkout, at most once.

    `--reverse --check` is the idempotence test, and a better one than a
    marker file: it succeeds only when the change is ALREADY present, so a
    re-run is a no-op, a checkout that moved under the patch fails loudly, and
    a file somebody edited by hand is detected rather than patched twice."""
    path = os.path.join(PATCHES, name)
    if not os.path.isfile(path):
        fail('[patch] no patch at "%s".' % path)
    if succeeds(["git", "-C", tree, "apply", "--reverse", "--check", path]):
        say("[patch] already patched: %s" % name)
        return
    say("[patch] patching: %s" % name)
    if subprocess.run(["git", "-C", tree, "apply", path]).returncode != 0:
        fail('[patch] could not apply "%s" in "%s".' % (path, tree),
             "[patch] the checkout moved under it; re-read the patch and rebase it.")


# --- vcpkg, for the mobile lanes -------------------------------------------------
def vcpkg_deps(vcpkg_root, triplet, dest, overlay=None):
    """The core's C++ dependencies for one triplet, via vcpkg, laid out in
    `dest` as include/, lib/, share/ and debug/.

    Manifest mode, from the repository's vcpkg.json. The dependency list lives
    there and nowhere else, so a port the core starts to need is added once
    and every lane installs it.

    The install root is per triplet rather than VCPKG_ROOT/installed. A
    manifest install may remove what its manifest does not ask for, and this
    vcpkg checkout is shared by every ABI of both mobile lanes.

    Re-running is idempotent: vcpkg skips ports already built, and `dest` is
    replaced."""
    # READ from vcpkg.json's "builtin-baseline" rather than repeating the hash
    # here: two copies of a pin is one copy that silently goes stale.
    manifest = os.path.join(ROOT, "vcpkg.json")
    try:
        with open(manifest, encoding="utf-8-sig") as f:
            baseline = json.load(f).get("builtin-baseline", "")
    except (OSError, ValueError) as e:
        fail("[deps] could not read %s: %s" % (manifest, e))
    if not baseline:
        fail('[deps] no "builtin-baseline" in "%s"' % manifest)

    say("[deps] triplet=%s  vcpkg=%s  baseline=%s" % (triplet, vcpkg_root, baseline))

    # Bootstrapped at the pinned baseline.
    if not os.path.isdir(os.path.join(vcpkg_root, ".git")):
        say("[deps] cloning vcpkg ...")
        run(["git", "clone", "https://github.com/microsoft/vcpkg", vcpkg_root])
    run(["git", "-C", vcpkg_root, "checkout", baseline])
    vcpkg = os.path.join(vcpkg_root, "vcpkg" + EXE)
    if not os.path.isfile(vcpkg):
        boot = "bootstrap-vcpkg." + ("bat" if WINDOWS else "sh")
        run([os.path.join(vcpkg_root, boot), "-disableMetrics"], cwd=vcpkg_root)

    install_root = os.path.join(vcpkg_root, "installed-" + triplet)
    say("[deps] installing from vcpkg.json into %s" % install_root)
    cmd = [vcpkg, "install", "--triplet", triplet,
           "--x-manifest-root=" + ROOT,
           "--x-install-root=" + install_root]
    if overlay:
        cmd.append("--overlay-triplets=" + overlay)
    cmd += ["--x-buildtrees-root", os.path.join(vcpkg_root, "buildtrees")]
    run(cmd)

    installed = os.path.join(install_root, triplet)
    if not os.path.isdir(installed):
        fail("[deps] expected install tree missing: %s" % installed)
    rmtree(dest)
    mkdirs(dest)
    # debug/ too: vcpkg's *Targets.cmake reference both release (lib/) and
    # debug (debug/lib/) imported locations, and CMake verifies both exist at
    # find_package time.
    for sub in ("include", "lib", "share", "debug"):
        if os.path.isdir(os.path.join(installed, sub)):
            copy_tree(os.path.join(installed, sub), os.path.join(dest, sub))
    say("[deps] done -> %s" % dest)


# =============================================================================
# The root build
#
# The point of a desktop build is that the core can be exercised without a
# device -- see cli/README.md. It is not a shipping target.
#
# `asr` assembles a desktop ggml SDK into build/llama-sdk/ from a llama.cpp
# checkout and configures with -DYOAI_ASR=ON. Only ggml is built --
# recognition does not link llama -- so this is a couple of minutes, not the
# twenty the full engine takes. `llama` builds that full engine into the same
# SDK, libllama on top of the same ggml, and configures with
# -DYOAI_ONDEVICE_LLM=ON.
#
# `tts` assembles a qwentts.cpp SDK into build/qtts-sdk/ and configures with
# -DYOAI_QTTS=ON -- speech output for the tools in cli/, without the desktop
# app.
# =============================================================================

BUILD = os.path.join(ROOT, "build")
WORDS = ("clean", "asr", "llama", "tts", "server", "cosyvoice")
# CMake caches which lanes are on; these are the cache entries that say so.
CACHED = (("asr", "YOAI_ASR"), ("llama", "YOAI_ONDEVICE_LLM"), ("tts", "YOAI_QTTS"),
          ("server", "YOAI_SERVER"))


def main(argv):
    want = set()
    for tok in argv:
        t = tok.lower()
        if t in ("help", "-h", "--help"):
            say(__doc__.strip())
            return 0
        if t not in WORDS:
            fail('Unknown argument "%s". Run %s --help for the options.' % (tok, script_cmd(__file__)))
        want.add(t)

    # After the arguments, so the help text still prints when a configured path
    # is wrong. The names are the paths this script reads.
    if WINDOWS:
        names = ("VS_DIR", "NINJA", "VCPKG_ROOT", "LLAMA_SRC", "QTTS_SRC", "COSYVOICE_SRC",
                 "VULKAN_SDK", "CUDA_PATH")
    else:
        names = ("VCPKG_ROOT", "OPENSSL_ROOT_DIR", "LLAMA_SRC", "QTTS_SRC", "COSYVOICE_SRC",
                 "VULKAN_SDK", "CUDA_PATH")
    if not load_env(*names):
        return 1

    if "clean" in want:
        rmtree(BUILD)

    # The generator: Ninja from Visual Studio on Windows, whatever CMake picks
    # everywhere else.
    gen = []
    if WINDOWS:
        gen = ["-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + visual_studio()]
    args = ["-DCMAKE_BUILD_TYPE=Release"] + vcpkg_args()

    # Adding `asr`, `llama` or `tts` to a tree that was configured without it
    # would change nothing. Said here, before any SDK is built, rather than
    # after minutes of building one.
    #
    # `server` is the exception: it has no SDK, only a package and a switch,
    # so the tree is configured again with it on instead. Everything else the
    # cache holds stays as it was.
    reconfigure = False
    if configured(BUILD):
        cache = read(os.path.join(BUILD, "CMakeCache.txt"))
        for lane, var in CACHED:
            if lane in want and var + ":BOOL=ON" not in cache:
                if lane == "server":
                    reconfigure = True
                    continue
                fail("[build] build/ was configured without %s; re-run as: %s clean %s"
                     % (lane, script_cmd(__file__), lane))

    # asr and llama share one SDK, so it is built once for either.
    if "asr" in want or "llama" in want:
        args.append("-DLLAMA_CPP_DIR=" + ggml_sdk(gen, "llama" in want))
        if "asr" in want:
            args.append("-DYOAI_ASR=ON")
        if "llama" in want:
            args.append("-DYOAI_ONDEVICE_LLM=ON")
    if "tts" in want:
        sdk = qtts_sdk(gen)
        args += ["-DYOAI_QTTS=ON", "-DQTTS_DIR=" + sdk]
        # CUDA follows what the SDK holds, as Vulkan does: the core finds the
        # ggml-cuda archive by name. The toolkit goes along, the same one that
        # compiled the archive, for the runtime libraries it needs. A tree
        # configured before the archive appeared, or after it went, is
        # configured again -- it would link libggml's reference to the CUDA
        # backend against nothing, or name an archive that is gone.
        with_cuda = has(os.path.join(sdk, "lib"), lib("ggml-cuda"))
        if with_cuda:
            args.append("-DCUDAToolkit_ROOT=" + fwd(os.path.dirname(os.path.dirname(nvcc()))))
        if configured(BUILD) and cached_lib(BUILD, "GGML_CUDA_LIB") != with_cuda:
            reconfigure = True
    if "server" in want:
        # The manifest feature is what makes vcpkg install libwebsockets; with
        # no vcpkg in play the variable is ignored and a distro package serves.
        args += ["-DYOAI_SERVER=ON", "-DVCPKG_MANIFEST_FEATURES=server"]
    if "cosyvoice" in want:
        cosyvoice(gen)

    if not configured(BUILD) or reconfigure:
        run(["cmake", "-S", ROOT, "-B", BUILD] + gen + args)
    run(["cmake", "--build", BUILD, "--config", "Release", "--parallel"])
    return 0


def vcpkg_args():
    """The vcpkg toolchain, when there is one to use."""
    vcpkg_root = env("VCPKG_ROOT")
    if WINDOWS:
        # Required here: without it CMake has no curl and no OpenSSL to find.
        # visual_studio() has already put the bundled copy in VCPKG_ROOT when
        # the user did not name one.
        toolchain = os.path.join(vcpkg_root, "scripts", "buildsystems", "vcpkg.cmake")
        if not vcpkg_root or not os.path.isfile(toolchain):
            fail('[build] no vcpkg at "%s". Set VCPKG_ROOT, or install the vcpkg '
                 "component of Visual Studio." % vcpkg_root)
        return ["-DCMAKE_TOOLCHAIN_FILE=" + toolchain,
                "-DVCPKG_TARGET_TRIPLET=x64-windows",
                "-DVCPKG_INSTALLED_DIR=" + os.path.join(ROOT, "vcpkg_installed")]

    # Elsewhere vcpkg is one way to get curl and openssl, not a requirement:
    # the CMakeLists resolves both with plain find_package, which a distro's own
    # packages satisfy just as well (apt install libcurl4-openssl-dev
    # libssl-dev). RapidJSON is vendored in third/.
    #
    # A VCPKG_ROOT the caller set is a request, not a hint: a wrong path is an
    # error rather than a silent fall back to the system packages, which would
    # build something other than what they asked for.
    explicit = bool(vcpkg_root)
    vcpkg_root = vcpkg_root or sibling("vcpkg")
    toolchain = os.path.join(vcpkg_root, "scripts", "buildsystems", "vcpkg.cmake")
    if os.path.isfile(toolchain):
        return ["-DCMAKE_TOOLCHAIN_FILE=" + toolchain,
                "-DVCPKG_INSTALLED_DIR=" + os.path.join(ROOT, "vcpkg_installed")]
    if explicit:
        fail("vcpkg not found at %s (from VCPKG_ROOT)." % vcpkg_root)
    return openssl_args()


def openssl_args():
    # macOS ships libcurl but no OpenSSL headers, and Homebrew leaves openssl@3
    # unlinked on purpose (it would shadow the system LibreSSL), so
    # find_package(OpenSSL) comes up empty on a machine that has it installed.
    # Only without vcpkg: with vcpkg in play, OpenSSL comes from there.
    if MACOS and not env("OPENSSL_ROOT_DIR"):
        prefix = output(["brew", "--prefix", "openssl@3"])
        if prefix and os.path.isdir(prefix):
            return ["-DOPENSSL_ROOT_DIR=" + prefix]
    return []


def llama_src(what):
    """The llama.cpp checkout whose ggml `what` compiles."""
    src = env("LLAMA_SRC", sibling("llama.cpp"))
    if not os.path.isfile(os.path.join(src, "ggml", "CMakeLists.txt")):
        fail("%s compiles llama.cpp's ggml, and there is none at %s." % (what, os.path.join(src, "ggml")),
             "  " + sync_cmd("clone", "llama.cpp"),
             "or point LLAMA_SRC at an existing checkout.")
    third_verify("llama.cpp", "LLAMA_SRC", src)
    return src


def ggml_sdk(gen, with_llama):
    """A desktop ggml SDK, shaped like the one harmony/build.py assembles per
    ABI: include/ plus lib/. Recognition links ggml and nothing else, so for
    `asr` alone only the three ggml targets are built. `llama` adds libllama
    and llama.h, built in the same tree on the same ggml."""
    src = env("LLAMA_SRC", sibling("llama.cpp"))
    if not os.path.isfile(os.path.join(src, "ggml", "include", "ggml.h")):
        fail("no llama.cpp at %s." % src,
             "clone it at the pinned commit, with its patch applied:",
             "  " + sync_cmd("clone", "llama.cpp"),
             "or point LLAMA_SRC at an existing checkout.")
    third_verify("llama.cpp", "LLAMA_SRC", src)

    gbuild = os.path.join(BUILD, "ggml")
    sdk = os.path.join(BUILD, "llama-sdk")
    targets = ["ggml", "ggml-cpu", "ggml-base"]
    if with_llama:
        targets.insert(0, "llama")
    say("[build] %s from %s" % (" ".join(targets), src))

    if not configured(gbuild):
        run(["cmake", "-S", src, "-B", gbuild] + gen + [
            "-DCMAKE_BUILD_TYPE=Release",
            "-DBUILD_SHARED_LIBS=OFF",
            "-DLLAMA_BUILD_TESTS=OFF",
            "-DLLAMA_BUILD_EXAMPLES=OFF",
            "-DLLAMA_BUILD_TOOLS=OFF",
            "-DLLAMA_BUILD_SERVER=OFF",
            "-DLLAMA_CURL=OFF"])
    run(["cmake", "--build", gbuild, "--parallel", "--target"] + targets)

    inc, libdir = os.path.join(sdk, "include"), os.path.join(sdk, "lib")
    mkdirs(inc, libdir)
    copy_headers_glob(os.path.join(src, "ggml", "include", "*.h"), inc)
    if with_llama:
        copy_headers([os.path.join(src, "include", "llama.h")], inc)
    copy_all(find(gbuild, *[lib(t) for t in ("llama", "ggml", "ggml-cpu", "ggml-base")]), libdir)
    # By search, so the check afterwards is what turns a silent miss into an
    # error.
    if not has(libdir, lib("ggml")):
        fail("[build] ggml built but nothing was copied into %s" % libdir)
    if with_llama and not has(libdir, lib("llama")):
        fail("[build] llama built but libllama was not copied into %s" % libdir)
    say("[build] llama.cpp SDK at %s" % sdk)
    return sdk


def qtts_sdk(gen):
    """A qwentts.cpp SDK for this build: include/ plus lib/, like the ggml SDK,
    so the core can speak without the desktop app. Its own copy rather than
    desktop/sdk/qtts-sdk, which on Windows is built /MT for cmake-js: this
    build is /MD like the vcpkg libraries it links, and the two do not mix in
    one link.

    qwentts.cpp and llama.cpp are both required. qwentts compiles llama.cpp's
    ggml through GGML_SOURCE_DIR, so the core carries one ggml however many
    lanes are on -- see "Exactly one ggml" in the root CMakeLists.txt.

    The GPU is not required. Without one this builds for the CPU and says so:
    the desktop measured the lane at RTF 1.13 on a laptop CPU against 0.63 on
    its integrated GPU, so it works, only slower than real time."""
    src = env("QTTS_SRC", sibling("qwentts.cpp"))
    if not os.path.isfile(os.path.join(src, "src", "qwen.h")):
        fail("no qwentts.cpp at %s." % src,
             "clone it at the pinned commit, submodule and patch included:",
             "  " + sync_cmd("clone", "qwentts.cpp"),
             "or point QTTS_SRC at an existing checkout.")
    third_verify("qwentts.cpp", "QTTS_SRC", src)
    ggml = llama_src("tts")

    # The backend, per platform, as in desktop/build.py: Metal on macOS, which
    # ggml turns on by itself for Apple; Vulkan elsewhere when there is one.
    # GGML_VULKAN is passed either way off macOS, and the tree is configured
    # every run, so the backend follows the machine rather than whatever the
    # first run cached.
    #
    # CUDA the same way, beside Vulkan rather than instead of it: one ggml can
    # hold both, so one build compares them on the same card
    # (tests/tts_rtf.py --backend Vulkan0 --backend CUDA0). ggml registers
    # CUDA first, so the engine's own pick is CUDA when both are in.
    vulkan_sdk = env("VULKAN_SDK")
    nvcc_path = None if MACOS else nvcc()
    vulkan = False
    cuda = nvcc_path is not None
    backend = []
    if MACOS:
        say("[build] tts backend: Metal, which ggml enables for Apple by itself")
    elif vulkan_sdk or (not WINDOWS and on_path("glslc")):
        vulkan = True
        backend = ["-DGGML_VULKAN=ON"] + spirv_args(vulkan_sdk)
        say("[build] tts backend: Vulkan" + (", from " + vulkan_sdk if vulkan_sdk else ""))
    else:
        backend = ["-DGGML_VULKAN=OFF"]
        if not cuda:
            warn("[build] warning: no Vulkan found, so tts is built for the CPU only.")
            warn("[build]   It works, slower: RTF 1.13 against 0.63 on the GPU where measured.")
            if WINDOWS:
                warn("[build]   For the GPU, install the Vulkan SDK or set YOAI_VULKAN_SDK in .env,")
            else:
                warn("[build]   For the GPU: apt install libvulkan-dev glslc (Debian/Ubuntu), or set")
                warn("[build]   YOAI_VULKAN_SDK in .env;")
            warn("[build]   then %s clean tts -- the core only looks for it when configured."
                 % script_cmd(__file__))

    # Built for THIS machine's card only, as the CPU half is built for this
    # machine's CPU. qwentts.cpp otherwise asks for five architectures, several
    # times the compile, for cards this tree will never run on. `native` needs
    # CMake 3.24; an older one keeps that list and only builds slower.
    #
    # NCCL off: it splits one model across several GPUs, and ggml links it
    # whenever it finds it, which the core's link line would then have to know.
    if cuda:
        backend += ["-DGGML_CUDA=ON", "-DGGML_CUDA_NCCL=OFF",
                    "-DCMAKE_CUDA_COMPILER=" + fwd(nvcc_path)]
        if cmake_at_least(3, 24):
            backend.append("-DCMAKE_CUDA_ARCHITECTURES=native")
        say("[build] tts backend: CUDA, from " + nvcc_path)
    elif not MACOS:
        backend.append("-DGGML_CUDA=OFF")

    qbuild = os.path.join(BUILD, "qtts")
    sdk = os.path.join(BUILD, "qtts-sdk")
    say("[build] qwentts.cpp from %s, ggml from %s" % (src, os.path.join(ggml, "ggml")))
    run(["cmake", "-S", src, "-B", qbuild] + gen + [
        "-DCMAKE_BUILD_TYPE=Release",
        "-DGGML_SOURCE_DIR=" + fwd(ggml) + "/ggml",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DQWEN_SHARED=OFF"] + backend)
    # qwen-core only: qwen-tts, qwen-codec and tts-server all want a main().
    run(["cmake", "--build", qbuild, "--config", "Release", "--parallel", "--target", "qwen-core"])

    # lib/ is laid out from empty every run. The libraries are collected by
    # search, because ggml splits its archives differently between versions,
    # and a tree that was once built with Vulkan or CUDA keeps that backend's
    # archive after the backend is turned off: the core links whatever this
    # directory holds, so those are taken out. include/ is kept, so that
    # copy_headers() can tell which headers changed.
    inc, libdir = os.path.join(sdk, "include"), os.path.join(sdk, "lib")
    rmtree(libdir)
    mkdirs(inc, libdir)
    copy_headers_glob(os.path.join(src, "src", "qwen*.h"), inc)
    patterns = ["*.lib"] if WINDOWS else ["libqwen-core.*", "libggml*.*"]
    copy_all(find(qbuild, *patterns), libdir)
    for name, on in (("ggml-vulkan", vulkan), ("ggml-cuda", cuda)):
        if not on:
            for p in find(libdir, lib(name)):
                os.remove(p)
    if not has(libdir, lib("qwen-core")):
        fail("[build] qwentts built, but no qwen-core library landed in %s" % libdir)
    if vulkan and not has(libdir, lib("ggml-vulkan")):
        fail("[build] qwentts built WITHOUT the Vulkan backend -- check that cmake found it")
    if cuda and not has(libdir, lib("ggml-cuda")):
        fail("[build] qwentts built WITHOUT the CUDA backend -- check that cmake found the toolkit")
    say("[build] tts SDK at %s" % sdk)
    return sdk


def cosyvoice(gen):
    """CosyVoice3's own command-line tool, for listening against Qwen3-TTS.
    Linked into nothing -- design/voice.md has why it is not a lane (RTF about
    4 on the CPU). It compiles llama.cpp's ggml like everything else. Its CMake
    downloads the latest ICU and ONNX Runtime releases into build/cosyvoice on
    the first configure; those two are not pinned.

    CPU only, and GGML_METAL off on every platform: with Metal on,
    cosyvoice.cpp patches the ggml it is given, which here is llama.cpp's
    tree."""
    src = env("COSYVOICE_SRC", sibling("cosyvoice.cpp"))
    if not os.path.isfile(os.path.join(src, "CMakeLists.txt")):
        fail("no cosyvoice.cpp at %s." % src,
             "  " + sync_cmd("clone", "cosyvoice.cpp"),
             "or point COSYVOICE_SRC at an existing checkout.")
    third_verify("cosyvoice.cpp", "COSYVOICE_SRC", src)
    ggml = llama_src("cosyvoice")

    cbuild = os.path.join(BUILD, "cosyvoice")
    run(["cmake", "-S", src, "-B", cbuild] + gen + [
        "-DCMAKE_BUILD_TYPE=Release",
        "-DGGML_SOURCE_DIR=" + fwd(ggml) + "/ggml",
        "-DGGML_VULKAN=OFF",
        "-DGGML_METAL=OFF"])
    run(["cmake", "--build", cbuild, "--parallel", "--target", "cosyvoice-cli"])
    say("[build] cosyvoice-cli at %s" % os.path.join(cbuild, "bin"))


if __name__ == "__main__":
    entry(main)
