#!/usr/bin/env python3
"""
Build the WebRTC AEC3 static library (GN + ninja).

Expected layout (produced by fetch_webrtc.py):
    webrtc/
    ├── .gclient
    └── src/            <- --src-dir

Examples:
    python build_aec3.py                                  # release, x64, bundled libaec3_bundle.a
    python build_aec3.py --debug                          # debug build
    python build_aec3.py --target-cpu arm64 --target-os android   # Linux/WSL2 host only
    python build_aec3.py --clang no                       # build with GCC
    python build_aec3.py --no-bundle                      # only libaec3.a (deps stay separate)
    python build_aec3.py --extra-dep //api/audio:echo_canceller3_factory
    python build_aec3.py --gn-arg rtc_use_h264=false --gn-arg is_asan=true
    python build_aec3.py -o ./dist                        # copy the library + flags.txt to ./dist
    python build_aec3.py --dry-run                        # print commands only
    python build_aec3.py --unpatch                        # remove the block added to src/BUILD.gn

Note: in bundle mode the script temporarily appends a clearly marked block to src/BUILD.gn
(a static_library with complete_static_lib = true and without the thin_archive config,
so the result is self-contained) and removes it again after a
successful build. Use --keep-patch to leave it in place, --unpatch to remove it manually.

For the shared library that yoai actually loads (yoai_aec3.dll / .so, plain C ABI) see
build_aec3_bridge.py, which reuses the helpers in this file.
"""
import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

DEFAULT_TARGET = "//modules/audio_processing/aec3:aec3"
BLOCK_BEGIN = "# >>> build_aec3.py begin (auto-generated, safe to delete)"
BLOCK_END = "# <<< build_aec3.py end"
DESC_KEYS = ("include_dirs", "defines", "cflags", "cflags_cc")

# Which target_os values can actually be built on each host OS (see WebRTC docs):
# Android only on Linux (use WSL2 on Windows), iOS only on macOS.
HOST_TARGETS = {
    "win": ["win"],
    "mac": ["mac", "ios"],
    "linux": ["linux", "android"],
}


def default_cpu(target_os):
    """Android/iOS default to arm64; desktop targets follow the host CPU architecture."""
    if target_os in ("android", "ios"):
        return "arm64"
    return "arm64" if platform.machine().lower() in ("arm64", "aarch64") else "x64"


def abs_gn_path(line, src: Path):
    """Turn a GN source-absolute path (//foo/bar) into an absolute filesystem path."""
    s = line.strip()
    if s.startswith("//"):
        return (src / s[2:]).as_posix().rstrip("/")
    return line


def log(msg):
    print(f"\n\033[1;36m==> {msg}\033[0m", flush=True)


def run(cmd, cwd=None, env=None, dry_run=False, capture=False):
    """Run a command with live output (or capture it). Exit on failure."""
    print(f"$ {' '.join(map(str, cmd))}", flush=True)
    if dry_run:
        return ""
    try:
        res = subprocess.run(
            cmd, cwd=cwd, env=env, check=True,
            capture_output=capture, text=capture,
        )
    except subprocess.CalledProcessError as e:
        sys.exit(f"Command failed (exit code {e.returncode}): {' '.join(map(str, cmd))}")
    return res.stdout if capture else ""


def resolve(tool, env):
    path = shutil.which(tool, path=env["PATH"])
    if not path:
        sys.exit(f"Could not find '{tool}'. Pass --depot-tools or add depot_tools to PATH.")
    return path


def gn_value(v):
    """Format a Python value as a GN literal."""
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    return f'"{v}"'


def ninja_name(label):
    """//a/b:c -> a/b:c (ninja target name)."""
    return label[2:] if label.startswith("//") else label


def lib_basenames(label_or_name):
    """Possible output file names for a GN target on Unix / Windows."""
    name = label_or_name.split(":")[-1]
    return (f"lib{name}.a", f"{name}.lib")


# --------------------------------------------------------------------------
# BUILD.gn patching (bundle mode)
# --------------------------------------------------------------------------
def strip_block(text):
    pattern = re.compile(re.escape(BLOCK_BEGIN) + r".*?" + re.escape(BLOCK_END) + r"(?:\r?\n)?", re.S)
    return pattern.sub("", text)


def read_gn(path: Path):
    """Read a file without newline translation; returns (text, newline_style)."""
    with open(path, "r", encoding="utf-8", newline="") as f:
        text = f.read()
    return text, ("\r\n" if "\r\n" in text else "\n")


def write_gn(path: Path, text):
    """Write a file without newline translation (keeps LF/CRLF exactly as given)."""
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def append_block(src: Path, block_lines, dry_run):
    """Put `block_lines` (wrapped in our begin/end markers) at the end of src/BUILD.gn,
    replacing any block of ours that is already there."""
    build_gn = src / "BUILD.gn"
    text, nl = read_gn(build_gn)
    text = strip_block(text)
    block = nl.join([BLOCK_BEGIN] + list(block_lines) + [BLOCK_END]) + nl
    log(f"Adding a block to {build_gn}")
    print(block)
    if not dry_run:
        write_gn(build_gn, text.rstrip("\r\n") + nl + nl + block)


def patch_build_gn(src: Path, name: str, deps, dry_run):
    append_block(
        src,
        [f'static_library("{name}") {{', "  complete_static_lib = true",
         '  configs -= [ "//build/config/compiler:thin_archive" ]', "  testonly = false", "  deps = ["]
        + [f'    "{d}",' for d in deps]
        + ["  ]", "}"],
        dry_run,
    )


def unpatch_build_gn(src: Path):
    build_gn = src / "BUILD.gn"
    text, nl = read_gn(build_gn)
    new_text = strip_block(text)
    if new_text == text:
        print("No build_aec3.py block found in BUILD.gn, nothing to do.")
        return
    write_gn(build_gn, new_text.rstrip("\r\n") + nl)
    print(f"Removed the build_aec3.py block from {build_gn}")


# --------------------------------------------------------------------------
# Main steps
# --------------------------------------------------------------------------
def make_env(src: Path, depot_tools=None):
    """Environment with depot_tools on PATH (default: <src>/../../depot_tools if it exists)."""
    env = os.environ.copy()
    depot = Path(depot_tools).expanduser().resolve() if depot_tools else src.parent.parent / "depot_tools"
    if depot.exists():
        env["PATH"] = str(depot) + os.pathsep + env["PATH"]
    env.setdefault("DEPOT_TOOLS_UPDATE", "0")
    if sys.platform == "win32":
        env.setdefault("DEPOT_TOOLS_WIN_TOOLCHAIN", "0")
    return env


def build_gn_args(a):
    args = {
        "is_debug": gn_value(a.debug),
        "is_component_build": gn_value(False),
        "target_os": gn_value(a.target_os),
        "target_cpu": gn_value(a.target_cpu),
        "rtc_include_tests": gn_value(False),
        "rtc_build_examples": gn_value(False),
        "rtc_build_tools": gn_value(False),
        "rtc_enable_protobuf": gn_value(False),
        "treat_warnings_as_errors": gn_value(False),
        "symbol_level": gn_value(a.symbol_level if a.symbol_level is not None else (2 if a.debug else 0)),
    }
    # use_custom_libcxx=false only makes sense on Linux (system C++ library). Android, iOS and
    # Windows all build against Chromium's bundled libc++ (std::__Cr), so keep the GN default
    # there unless --custom-libcxx is given explicitly.
    if a.target_os == "linux" or a.custom_libcxx:
        args["use_custom_libcxx"] = gn_value(a.custom_libcxx)
    if a.clang != "auto":
        args["is_clang"] = gn_value(a.clang == "yes")
    # User-supplied KEY=VALUE pairs are passed through verbatim and override defaults
    for item in a.gn_arg or []:
        if "=" not in item:
            sys.exit(f"Invalid --gn-arg '{item}', expected KEY=VALUE")
        k, v = item.split("=", 1)
        args[k.strip()] = v.strip()
    return " ".join(f"{k}={v}" for k, v in args.items())


def find_artifact(out_dir: Path, names):
    for n in names:
        hits = sorted((out_dir / "obj").rglob(n))
        if hits:
            return hits[0]
    return None


def dump_flags(src, out_rel, label, env, dest: Path):
    gn = resolve("gn", env)
    lines = [
        f"# GN flags for {label}",
        f"# out dir: {out_rel}",
        f"# include_dirs: '//' paths converted to absolute paths under {src.as_posix()}",
        "",
    ]
    for key in DESC_KEYS:
        out = run([gn, "desc", out_rel, label, key], cwd=src, env=env, capture=True).strip()
        if key == "include_dirs":
            out = "\n".join(abs_gn_path(line, src) for line in out.splitlines())
        lines += [f"[{key}]", out, ""]
    dest.write_text("\n".join(lines), encoding="utf-8")
    print(f"Wrote {dest}")


def main():
    p = argparse.ArgumentParser(description="Build the WebRTC AEC3 static library")
    p.add_argument("-s", "--src-dir", default="webrtc/src", help="WebRTC src directory (default: webrtc/src)")
    p.add_argument("--depot-tools", default=None, help="depot_tools directory (default: <src>/../../depot_tools if it exists, else PATH)")
    p.add_argument("--out-dir", default="out/aec3", help="GN output dir, relative to --src-dir (default: out/aec3)")
    p.add_argument("-o", "--output-dir", default=None, help="copy the final library (and flags.txt) here")
    p.add_argument("--target", default=DEFAULT_TARGET, help=f"GN label to build (default: {DEFAULT_TARGET})")

    g = p.add_argument_group("GN configuration")
    g.add_argument("--debug", action="store_true", help="debug build (default: release)")
    g.add_argument("--target-os", default=None, help="GN target_os; allowed values depend on the host OS: Windows -> win, macOS -> mac/ios, Linux -> linux/android (default: host OS)")
    g.add_argument("--target-cpu", default=None, help="GN target_cpu: x64, x86, arm, arm64 (default: arm64 for android/ios, otherwise the host CPU)")
    g.add_argument("--clang", choices=["auto", "yes", "no"], default="auto", help="is_clang (default: auto = GN default)")
    g.add_argument("--custom-libcxx", action="store_true", help="use WebRTC's bundled libc++ (default: off on Linux; other targets keep the GN default)")
    g.add_argument("--symbol-level", type=int, default=None, help="GN symbol_level (default: 0 release, 2 debug)")
    g.add_argument("--gn-arg", action="append", metavar="KEY=VALUE", help="extra GN arg, repeatable (overrides defaults)")

    b = p.add_argument_group("bundling")
    b.add_argument("--no-bundle", action="store_true", help="do not create a bundled library; build only the target itself")
    b.add_argument("--bundle-name", default="aec3_bundle", help="name of the bundled target (default: aec3_bundle)")
    b.add_argument("--extra-dep", action="append", metavar="LABEL", help="extra GN dep to include in the bundle, repeatable")
    b.add_argument("--keep-patch", action="store_true", help="keep the bundle target in src/BUILD.gn after the build (default: remove it again)")

    p.add_argument("-j", "--jobs", type=int, default=None, help="ninja parallel jobs (default: ninja decides)")
    p.add_argument("--clean", action="store_true", help="delete the output dir before generating")
    p.add_argument("--no-flags", action="store_true", help="do not write flags.txt when using --output-dir")
    p.add_argument("--unpatch", action="store_true", help="remove the block this script added to src/BUILD.gn, then exit")
    p.add_argument("--dry-run", action="store_true", help="print commands without executing them")
    a = p.parse_args()

    host_os = {"win32": "win", "darwin": "mac"}.get(sys.platform, "linux")
    if a.target_os is None:
        a.target_os = host_os
    elif a.target_os not in HOST_TARGETS[host_os]:
        p.error(
            f"'--target-os {a.target_os}' cannot be built on a {host_os} host. "
            f"Allowed on this host: {', '.join(HOST_TARGETS[host_os])}. "
            "(Android needs Linux, e.g. WSL2 on Windows; iOS needs macOS.)"
        )
    if a.target_cpu is None:
        a.target_cpu = default_cpu(a.target_os)

    src = Path(a.src_dir).expanduser().resolve()
    if not (src / "BUILD.gn").exists() or not (src / ".gn").exists():
        sys.exit(f"{src} does not look like a WebRTC src directory (BUILD.gn / .gn missing).")

    if a.unpatch:
        unpatch_build_gn(src)
        return

    if not (src / "build").is_dir():
        print("[!] src/build is missing. Did 'gclient sync' finish? GN may fail.", flush=True)

    # Environment: put depot_tools on PATH
    env = make_env(src, a.depot_tools)

    out_rel = a.out_dir.replace("\\", "/")
    out_abs = src / out_rel

    # 1. Optional clean
    if a.clean and out_abs.exists():
        log(f"Removing {out_abs}")
        if not a.dry_run:
            shutil.rmtree(out_abs)

    # 2. Bundle target in BUILD.gn (must exist before `gn gen`)
    ninja_targets = [ninja_name(a.target)]
    expected_names = lib_basenames(a.target)
    if not a.no_bundle:
        deps = [a.target] + (a.extra_dep or [])
        patch_build_gn(src, a.bundle_name, deps, a.dry_run)
        ninja_targets = [a.bundle_name]
        expected_names = lib_basenames(a.bundle_name)

    # 3. gn gen
    log("gn gen")
    gn_args = build_gn_args(a)
    print(f"GN args: {gn_args}")
    run([resolve("gn", env), "gen", out_rel, f"--args={gn_args}"], cwd=src, env=env, dry_run=a.dry_run)

    # 4. ninja
    log(f"ninja {' '.join(ninja_targets)}")
    cmd = [resolve("ninja", env), "-C", out_rel]
    if a.jobs:
        cmd += ["-j", str(a.jobs)]
    run(cmd + ninja_targets, cwd=src, env=env, dry_run=a.dry_run)

    if a.dry_run:
        log("Dry run finished")
        return

    # 5. Locate the artifact
    lib = find_artifact(out_abs, expected_names)
    if not lib:
        sys.exit(f"Build finished but {' / '.join(expected_names)} was not found under {out_abs / 'obj'}")

    # 6. Copy results and dump compile flags
    if a.output_dir:
        dest = Path(a.output_dir).expanduser().resolve()
        dest.mkdir(parents=True, exist_ok=True)
        shutil.copy2(lib, dest / lib.name)
        print(f"Copied {lib.name} -> {dest}")
        if not a.no_flags:
            dump_flags(src, out_rel, a.target, env, dest / "flags.txt")

    size = lib.stat().st_size
    with open(lib, "rb") as f:
        is_thin = f.read(8) == b"!<thin>\n"
    if is_thin and (not a.no_bundle or a.output_dir):
        print(
            f"[!] {lib.name} is a thin archive: it only references object files under {out_abs / 'obj'} "
            "and does not contain the code itself, so a copy elsewhere will not link.",
            flush=True,
        )

    # The bundle target was only needed for this build; leave the git-tracked BUILD.gn clean
    if not a.no_bundle and not a.keep_patch:
        log("Removing the temporary bundle target from BUILD.gn")
        unpatch_build_gn(src)

    log("Done")
    print(f"Library: {lib} ({size / 1024:.0f} KiB)")
    if a.no_bundle:
        print("Note: dependencies (rtc_base, api, abseil, ...) are separate .a files under obj/.")
    print("To get include paths / defines for your project, see flags.txt (use -o) or run:")
    print(f"  gn desc {out_rel} {a.target} include_dirs   (and defines, cflags_cc)")


if __name__ == "__main__":
    main()