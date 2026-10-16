#!/usr/bin/env python3
"""
One-shot WebRTC source checkout (with the src/ subdirectory).

Resulting layout:
    <dir>/
    ├── .gclient
    └── src/

Examples:
    python fetch_webrtc.py                              # check out into ./webrtc
    python fetch_webrtc.py -d ~/webrtc --full-history   # keep full git history (much larger)
    python fetch_webrtc.py -b 6367                      # switch to branch-heads/6367
    python fetch_webrtc.py -p android                   # Android dependencies, require linux
    python fetch_webrtc.py --proxy http://127.0.0.1:7890
    python fetch_webrtc.py --manual                     # skip fetch: git clone + write .gclient
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

DEPOT_TOOLS_URL = "https://chromium.googlesource.com/chromium/tools/depot_tools.git"
WEBRTC_URL = "https://webrtc.googlesource.com/src.git"

GCLIENT_TEMPLATE = """solutions = [
  {
    "name": "src",
    "url": "%s",
    "deps_file": "DEPS",
    "managed": False,
    "custom_deps": {},
  },
]
"""

FETCH_TARGETS = {
    "linux": "webrtc",
    "mac": "webrtc",
    "win": "webrtc",
    "android": "webrtc_android",
    "ios": "webrtc_ios",
}

# Which targets can actually be built on each host OS (see WebRTC docs):
# Android only on Linux (use WSL2 on Windows), iOS only on macOS.
HOST_TARGETS = {
    "win": ["win"],
    "mac": ["mac", "ios"],
    "linux": ["linux", "android"],
}


def detect_platform():
    """Detect the host OS and map it to a platform key in FETCH_TARGETS."""
    if sys.platform == "win32":
        return "win"
    if sys.platform == "darwin":
        return "mac"
    return "linux"


def log(msg):
    print(f"\n\033[1;36m==> {msg}\033[0m", flush=True)


def run(cmd, cwd=None, env=None, retries=0):
    """Run a command with live output; retry on failure."""
    attempt = 0
    while True:
        attempt += 1
        print(f"$ {' '.join(map(str, cmd))}", flush=True)
        try:
            subprocess.run(cmd, cwd=cwd, env=env, check=True)
            return
        except subprocess.CalledProcessError as e:
            if attempt > retries:
                sys.exit(f"Command failed (exit code {e.returncode}): {' '.join(map(str, cmd))}")
            print(f"[!] Failed, retry {attempt}/{retries}...", flush=True)


def resolve(tool, env):
    """Locate an executable on the PATH that includes depot_tools (handles .bat on Windows)."""
    path = shutil.which(tool, path=env["PATH"])
    if not path:
        sys.exit(f"Could not find '{tool}'. Check that depot_tools was installed correctly.")
    return path


def ensure_depot_tools(depot_dir: Path, env):
    if not depot_dir.exists():
        log(f"Cloning depot_tools -> {depot_dir}")
        run(["git", "clone", DEPOT_TOOLS_URL, str(depot_dir)], env=env, retries=2)
    else:
        log(f"depot_tools already exists: {depot_dir}")
    env["PATH"] = str(depot_dir) + os.pathsep + env["PATH"]
    # Don't auto-update depot_tools (avoids a slow first run); run `git pull` manually if needed
    env.setdefault("DEPOT_TOOLS_UPDATE", "0")
    if sys.platform == "win32":
        # Use the locally installed Visual Studio instead of downloading Google's internal toolchain
        env.setdefault("DEPOT_TOOLS_WIN_TOOLCHAIN", "0")


def checkout_with_fetch(work_dir: Path, platform: str, no_history: bool, env, retries):
    target = FETCH_TARGETS[platform]
    if (work_dir / ".gclient").exists() and (work_dir / "src").exists():
        log(".gclient and src/ already exist, skipping fetch")
        return
    log(f"fetch {target} (directory: {work_dir})")
    cmd = [resolve("fetch", env), "--nohooks"]
    if no_history:
        cmd.append("--no-history")
    cmd.append(target)
    run(cmd, cwd=work_dir, env=env, retries=retries)


def checkout_manual(work_dir: Path, platform: str, shallow: bool, env, retries):
    src = work_dir / "src"
    gclient_file = work_dir / ".gclient"
    if not src.exists():
        log("git clone WebRTC -> src/")
        cmd = ["git", "clone"]
        if shallow:
            cmd += ["--depth", "1"]
        run(cmd + [WEBRTC_URL, str(src)], env=env, retries=retries)
    if not gclient_file.exists():
        log("Writing .gclient")
        content = GCLIENT_TEMPLATE % WEBRTC_URL
        if platform in ("android", "ios"):
            content += f'target_os = ["{platform}"]\n'
        gclient_file.write_text(content, encoding="utf-8")


def switch_ref(src: Path, ref: str, shallow: bool, env):
    """Switch to a branch / tag / commit. A bare number or branch-heads/NNNN is treated as an M-release branch.

    With shallow=True only the single target commit is downloaded (--depth 1).
    """
    m = re.fullmatch(r"(?:branch-heads/)?(\d{4,5})", ref)
    remote_ref = f"refs/branch-heads/{m.group(1)}" if m else ref
    log(f"Switching to {ref}")
    cmd = ["git", "fetch"]
    if shallow:
        cmd += ["--depth", "1"]
    run(cmd + ["origin", remote_ref], cwd=src, env=env, retries=2)
    run(["git", "checkout", "FETCH_HEAD"], cwd=src, env=env)


def check_long_paths(enable: bool):
    """Windows only: warn if git core.longpaths or Win32 long path support is off.

    With enable=True, git core.longpaths is turned on (--global, no admin needed).
    The Win32 registry setting needs admin rights, so it is only reported, never changed.
    """
    if sys.platform != "win32":
        return

    r = subprocess.run(["git", "config", "--get", "core.longpaths"], capture_output=True, text=True)
    if r.stdout.strip().lower() != "true":
        if enable:
            log("Enabling git core.longpaths (--global)")
            run(["git", "config", "--global", "core.longpaths", "true"])
        else:
            print(
                "[!] git core.longpaths is not enabled; WebRTC's deep paths may fail to check out.\n"
                "    Fix: git config --global core.longpaths true   (or re-run without --no-longpaths)",
                flush=True,
            )

    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\FileSystem") as key:
            win_ok = winreg.QueryValueEx(key, "LongPathsEnabled")[0] == 1
    except OSError:
        win_ok = False
    if not win_ok:
        print(
            "[!] Windows long path support (LongPathsEnabled) is not enabled.\n"
            "    Fix (admin PowerShell): New-ItemProperty -Path "
            "'HKLM:\\SYSTEM\\CurrentControlSet\\Control\\FileSystem' "
            "-Name LongPathsEnabled -Value 1 -PropertyType DWORD -Force",
            flush=True,
        )


def main():
    p = argparse.ArgumentParser(description="One-shot WebRTC source checkout (with src/ subdirectory)")
    p.add_argument("-d", "--dir", default="webrtc", help="checkout directory (default: webrtc)")
    p.add_argument("--depot-tools", default=None, help="depot_tools directory (default: <dir>/../depot_tools)")
    p.add_argument("-b", "--branch", default=None, help="branch/version, e.g. 6367, branch-heads/6367, a tag or a commit")
    p.add_argument("-p", "--platform", choices=FETCH_TARGETS.keys(), default=None,
                   help="target platform; allowed values depend on the host OS: Windows -> win, macOS -> mac/ios, Linux -> linux/android (default: the host OS)")
    p.add_argument("--full-history", action="store_true", help="keep full git history (default: shallow checkout without history, much smaller)")
    p.add_argument("--manual", action="store_true", help="git clone + write .gclient instead of using fetch")
    p.add_argument("--no-sync", action="store_true", help="only fetch the source, skip gclient sync")
    p.add_argument("--retries", type=int, default=3, help="retries for failed network commands (default: 3)")
    p.add_argument("--proxy", default=None, help="HTTP(S) proxy, e.g. http://127.0.0.1:7890")
    p.add_argument("--no-longpaths", action="store_true", help="Windows: don't touch git config; only warn if core.longpaths is off (default: set core.longpaths=true --global)")
    args = p.parse_args()

    host = detect_platform()
    if args.platform is None:
        args.platform = host
        print(f"Auto-detected platform: {args.platform}")
    elif args.platform not in HOST_TARGETS[host]:
        p.error(
            f"'-p {args.platform}' cannot be built on a {host} host. "
            f"Allowed on this host: {', '.join(HOST_TARGETS[host])}. "
            "(Android needs Linux, e.g. WSL2 on Windows; iOS needs macOS.)"
        )

    if not shutil.which("git"):
        sys.exit("git not found. Please install git first.")

    check_long_paths(not args.no_longpaths)

    # Default: only the latest commit of every repo (shallow); --full-history disables this
    shallow = not args.full_history

    work_dir = Path(args.dir).expanduser().resolve()
    depot_dir = (
        Path(args.depot_tools).expanduser().resolve()
        if args.depot_tools
        else work_dir.parent / "depot_tools"
    )

    env = os.environ.copy()
    if args.proxy:
        for k in ("HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy"):
            env[k] = args.proxy

    # 1. depot_tools
    ensure_depot_tools(depot_dir, env)

    # 2. Create the checkout directory and fetch the source
    work_dir.mkdir(parents=True, exist_ok=True)
    if args.manual:
        checkout_manual(work_dir, args.platform, shallow, env, args.retries)
    else:
        checkout_with_fetch(work_dir, args.platform, shallow, env, args.retries)

    src = work_dir / "src"
    if not src.exists():
        sys.exit("src/ directory does not exist; fetch may have failed.")

    # 3. Switch to the requested version
    if args.branch:
        switch_ref(src, args.branch, shallow, env)

    # 4. Sync dependencies
    if not args.no_sync:
        log("gclient sync")
        cmd = [resolve("gclient", env), "sync"]
        if shallow:
            cmd.append("--no-history")
        if args.branch:
            # branch-heads refs are only needed (and only downloaded) when pinning to a release branch
            cmd += ["-D", "--force", "--reset", "--with_branch_heads"]
        run(cmd, cwd=work_dir, env=env, retries=args.retries)

    log("Done")
    print(f"Source directory: {src}")
    print(f"depot_tools:      {depot_dir}")
    print("\nBefore using it, add depot_tools to your PATH:")
    if sys.platform == "win32":
        print(f'  set PATH={depot_dir};%PATH%')
    else:
        print(f'  export PATH="$PATH:{depot_dir}"')


if __name__ == "__main__":
    main()