#!/usr/bin/env python3
"""The external checkouts this repo builds against: clone them, report on them,
or move them forward. manifest.json is the record; this reads it. README.md
says what each of its fields means.

    python third/sync.py [check|clone|update|verify] [name ...]

  check    (default) say what is there, whether it is on the pinned commit,
           whether the patches are applied, whether anything else is in the
           tree. Changes nothing.
  clone    clone whatever is missing at its pinned commit, init submodules,
           apply patches. Existing checkouts are left alone.
  update   move each checkout to its branch tip and rewrite the commit values
           in manifest.json with the new pins. Patches come off first and go
           back on after, so one that no longer fits STOPS that dependency
           rather than leaving it half-moved.
  verify   for the build scripts: silent and exit 0 when each checkout is on
           its pinned commit with every patch applied; otherwise one line per
           problem on stderr and exit 1. Changes nothing. A missing checkout
           counts as a problem. Stray files do not -- a patched tree always
           has some, so they are check's business, not this.

  name ... limit to these (the manifest's top-level names, case-insensitive).
           No name means all of them.

Checkouts are siblings of this repo, not children of it. That is not taste:
they are multi-gigabyte trees carrying their own build output, and one
`git clean -xdf` in here would delete them. Each one's location can be
overridden by the same variable the build scripts already read -- LLAMA_SRC,
MNN_SRC, QTTS_SRC, COSYVOICE_SRC, VCPKG_ROOT -- so this script and the builds
cannot end up pointing at different trees.

Env overrides:
  THIRD_ROOT   where checkouts live (default: this repo's parent directory)
  <dep>_SRC    one checkout's exact path; see each entry's env in manifest.json
  Each <dep>_SRC also reads as YOAI_<dep>_SRC, from the environment or from
  .env at the repository root -- the same rules the build scripts load by.

Python 3.8 or newer, standard library only.
"""

import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MANIFEST = os.path.join(HERE, "manifest.json")
PATCHDIR = os.path.join(HERE, "patches")

# The .env loader is the root build.py's, so the builds and this script read
# settings by the same rules. Loaded by path, under a name of its own, as the
# build scripts load it.
sys.dont_write_bytecode = True  # no __pycache__ in the repository
_spec = importlib.util.spec_from_file_location("yoai_build", os.path.join(REPO, "build.py"))
build = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(build)

MODES = ("check", "clone", "update", "verify")
# Each field and the JSON type it must have.
KEYS = {"env": str, "url": str, "branch": str, "commit": str,
        "submodules": bool, "patches": list, "used_by": str}
REQUIRED = ("env", "url", "branch", "commit")
# A pin is a commit, never a tag or a branch: forty hex digits, or sixty-four
# in a SHA-256 repository.
HASH = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")


def say(*args):
    # Flushed, because git writes to the same console straight from its own
    # process and would otherwise overtake what is still buffered here.
    print(*args, flush=True)


def warn(*args):
    print(*args, file=sys.stderr, flush=True)


class Dep:
    def __init__(self, name, fields):
        self.name = name
        self.env = fields["env"]
        self.url = fields["url"]
        self.branch = fields["branch"]
        self.commit = fields["commit"]
        self.pinned = self.commit  # as read, so `update` knows what moved
        self.submodules = fields.get("submodules", False)
        self.patches = fields.get("patches", [])  # file@worktree, in order
        self.used_by = fields.get("used_by", "-")
        self.dir = ""


def _no_duplicates(pairs):
    # json keeps the last of two equal keys without a word; a second entry of
    # the same name, or a field given twice, is a mistake worth stopping on.
    seen = {}
    for key, val in pairs:
        if key in seen:
            raise ValueError('"%s" appears twice' % key)
        seen[key] = val
    return seen


# --- The table ----------------------------------------------------------------
# Read before the arguments, so a name on the command line can be checked
# against it. A key the script does not know is an error, so a typo fails
# instead of quietly dropping a value.
def read_manifest():
    try:
        with open(MANIFEST, encoding="utf-8-sig") as f:
            table = json.load(f, object_pairs_hook=_no_duplicates)
    except (OSError, ValueError) as e:
        warn("manifest.json: %s" % e)
        return None
    if not isinstance(table, dict) or not table:
        warn("manifest.json must be an object with one entry per checkout.")
        return None

    deps, bad = [], False
    for name, fields in table.items():
        if not isinstance(fields, dict):
            warn('manifest.json: "%s" is not an object.' % name)
            bad = True
            continue
        for key, val in fields.items():
            if key not in KEYS:
                warn('manifest.json: "%s" has an unknown key "%s".' % (name, key))
                bad = True
            elif not isinstance(val, KEYS[key]):
                warn('manifest.json: "%s" %s must be a %s.' % (name, key, KEYS[key].__name__))
                bad = True
        for key in REQUIRED:
            if not fields.get(key):
                warn('manifest.json: "%s" has no %s.' % (name, key))
                bad = True
        if isinstance(fields.get("commit"), str) and not HASH.match(fields["commit"]):
            warn('manifest.json: "%s" commit is not a full hash: %s' % (name, fields["commit"]))
            bad = True
        patches = fields.get("patches", [])
        if isinstance(patches, list) and not all(isinstance(p, str) and p for p in patches):
            warn('manifest.json: "%s" patches must be file names.' % name)
            bad = True
        if not bad:
            deps.append(Dep(name, fields))
    return None if bad else deps


# --- Git -----------------------------------------------------------------------
def git(*args, quiet=False):
    out = subprocess.DEVNULL if quiet else None
    return subprocess.run(("git",) + args, stdout=out, stderr=out).returncode == 0


def git_out(*args):
    r = subprocess.run(("git",) + args, stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL, universal_newlines=True)
    return r.stdout if r.returncode == 0 else ""


def patch_path(spec, dep):
    """The patch file and the work tree it applies in, from file@worktree."""
    file, _, sub = spec.partition("@")
    tree = dep.dir if sub in ("", ".") else os.path.join(dep.dir, sub)
    return file, os.path.join(PATCHDIR, file), tree


def is_applied(path, tree):
    # `--reverse --check` is the idempotence test, and a better one than a
    # marker file: it succeeds only when the change is ALREADY present. So
    # applying twice is a no-op, a checkout that moved under a patch fails
    # loudly, and a file somebody edited by hand is detected rather than
    # patched on top of.
    return git("-C", tree, "apply", "--reverse", "--check", path, quiet=True)


# --- Patches -------------------------------------------------------------------
# Every patch for one dependency, in manifest order. Stops at the first
# failure: half-patching a tree is worse than not touching it.
def do_patches(dep, act):
    for spec in dep.patches:
        file, path, tree = patch_path(spec, dep)
        if not os.path.isfile(path):
            warn("    no patch at %s" % path)
            return False
        if is_applied(path, tree):
            if act == "apply":
                say("    patch ok: %s" % file)
            else:
                if not git("-C", tree, "apply", "--reverse", path):
                    warn("    could not reverse %s in %s" % (file, tree))
                    return False
                say("    un-patched: %s" % file)
        elif act == "apply":
            if not git("-C", tree, "apply", path):
                warn("    COULD NOT APPLY %s in %s -- rebase it against the new tree." % (file, tree))
                return False
            say("    patched: %s" % file)
    return True


# --- Clone ---------------------------------------------------------------------
# At the pinned commit. Not --depth 1 and not --branch: the pin is a commit
# rather than a tag, and it is usually behind the tip, so neither shortcut can
# check it out.
def do_clone(dep):
    say("    cloning...")
    if not git("clone", dep.url, dep.dir):
        return False
    if not git("-C", dep.dir, "checkout", "--detach", dep.commit):
        return False
    if dep.submodules and not git("-C", dep.dir, "submodule", "update", "--init", "--recursive"):
        return False
    return do_patches(dep, "apply")


# --- Update --------------------------------------------------------------------
# Move to the branch tip. The patches come off before the fetch: pulling over
# a patched file either conflicts or silently merges, and neither is a thing
# to discover during a build three days later.
def do_update(dep):
    if not do_patches(dep, "reverse"):
        return False
    say("    fetching...")
    if not git("-C", dep.dir, "fetch", "origin", dep.branch):
        return False
    if not git("-C", dep.dir, "checkout", "--detach", "FETCH_HEAD"):
        return False
    if dep.submodules and not git("-C", dep.dir, "submodule", "update", "--init", "--recursive"):
        return False
    dep.commit = git_out("-C", dep.dir, "rev-parse", "HEAD").strip()
    return do_patches(dep, "apply")


# --- Report --------------------------------------------------------------------
# What is there, against what the manifest says. Three facts reported
# separately because they go wrong separately: the commit, the patches, and
# whatever else somebody left in the tree.
def do_report(dep):
    head = git_out("-C", dep.dir, "rev-parse", "HEAD").strip()
    if head == dep.commit:
        say("    commit %s (pinned)" % head[:12])
    else:
        say("    commit %s -- manifest pins %s" % (head[:12], dep.commit[:12]))
    for spec in dep.patches:
        file, path, tree = patch_path(spec, dep)
        say("    patch %s: %s" % ("ok" if is_applied(path, tree) else "MISSING", file))
    # Anything beyond the patches. A patched file is EXPECTED to show as
    # modified, so this counts paths instead of asserting a clean tree: it is
    # a prompt to look, not a failure.
    ndirty = len(git_out("-C", dep.dir, "status", "--porcelain").splitlines())
    if ndirty:
        say('    %d modified/untracked path(s) -- git -C "%s" status' % (ndirty, dep.dir))


# --- Verify --------------------------------------------------------------------
# The same two facts do_report states, said only when they are wrong, so a
# build can run this every time and print nothing on the normal path.
def do_verify(dep):
    ok = True
    head = git_out("-C", dep.dir, "rev-parse", "HEAD").strip()
    if head != dep.commit:
        warn("[third] %s: commit %s, manifest pins %s" % (dep.name, head[:12], dep.commit[:12]))
        ok = False
    for spec in dep.patches:
        file, path, tree = patch_path(spec, dep)
        if not is_applied(path, tree):
            warn("[third] %s: patch MISSING: %s" % (dep.name, file))
            ok = False
    return ok


# --- The rewrite ---------------------------------------------------------------
# Put the new pins into manifest.json, and touch nothing else: only the commit
# value of an entry whose checkout moved is changed, and every other byte --
# key order, indentation, line endings -- is copied through, which json.dump
# would not do. The result is parsed back before it replaces the file, so a
# replacement that landed anywhere but the right commit fails here instead.
def rewrite(deps):
    moved = [d for d in deps if d.commit != d.pinned]
    if not moved:
        say()
        say("[third] every checkout was already at its pin; manifest.json unchanged")
        return True
    with open(MANIFEST, encoding="utf-8", newline="") as f:
        text = f.read()

    trouble = []
    for d in moved:
        old = '"%s"' % d.pinned
        # A pin shared by two entries could be either one's, so it is not
        # guessed at.
        if text.count(old) != 1:
            trouble.append(d.name)
        else:
            text = text.replace(old, '"%s"' % d.commit)
    if not trouble:
        try:
            table = json.loads(text)
            trouble = [d.name for d in moved if table[d.name]["commit"] != d.commit]
        except (ValueError, KeyError, TypeError):
            trouble = [d.name for d in moved]
    if trouble:
        warn("[third] could not rewrite the commit of: %s; manifest.json left as it was. New pins:"
             % " ".join(trouble))
        for d in moved:
            warn("    %s %s" % (d.name, d.commit))
        return False

    tmp = MANIFEST + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    os.replace(tmp, MANIFEST)
    say()
    say("[third] manifest.json rewritten with the new pins -- git diff third/manifest.json")
    return True


def main(argv):
    try:
        sys.stdout.reconfigure(errors="replace")
        sys.stderr.reconfigure(errors="replace")
    except AttributeError:
        pass

    # Before any default below, so a path set in .env is the one cloned into.
    # No names: a clone target does not exist yet, and a missing checkout is
    # what check and verify report.
    build.load_env()
    third_root = os.environ.get("THIRD_ROOT") or os.path.dirname(REPO)

    if shutil.which("git") is None:
        warn("need git on PATH")
        return 1
    deps = read_manifest()
    if deps is None:
        return 1

    # One mode keyword plus any number of dependency names, in any order. An
    # unrecognized token is an error, not a guess: quietly doing `check`
    # because somebody typed `updte` is the failure worth designing out.
    by_name = {d.name.lower(): d for d in deps}
    mode, only, bad = "check", set(), False
    for tok in argv:
        if tok in MODES:
            mode = tok
        elif tok.lower() in by_name:
            only.add(tok.lower())
        else:
            warn("unknown argument: %s" % tok)
            bad = True
    if bad:
        warn("  modes: %s" % " | ".join(MODES))
        warn("  names: %s" % " ".join(d.name for d in deps))
        return 1

    if mode != "verify":
        say("[third] root=%s  mode=%s" % (third_root, mode))
        say()

    failed = []
    for dep in deps:
        if only and dep.name.lower() not in only:
            continue
        # Where it lives: the build scripts' own variable wins, so the two can
        # never disagree about which tree is being built.
        dep.dir = os.environ.get(dep.env) or os.path.join(third_root, dep.name)
        present = os.path.isdir(os.path.join(dep.dir, ".git"))

        if mode == "verify":
            if not present:
                warn("[third] %s: MISSING at %s" % (dep.name, dep.dir))
                failed.append(dep.name)
            elif not do_verify(dep):
                failed.append(dep.name)
            continue

        say("=== %s  (%s)" % (dep.name, dep.used_by))
        say("    %s" % dep.dir)
        if not present:
            if mode != "clone":
                say("    MISSING -- `python third/sync.py clone %s` would fetch it" % dep.name)
                say()
                continue
            if not do_clone(dep):
                failed.append(dep.name)
                say()
                continue
        if mode == "update" and not do_update(dep):
            failed.append(dep.name)
            say()
            continue
        do_report(dep)
        say()

    if mode == "update" and not rewrite(deps):
        failed.append("manifest.json")

    # verify says nothing on success and has already named each problem.
    if mode == "verify":
        return 1 if failed else 0
    if failed:
        say()
        say("[third] trouble with: %s" % " ".join(failed))
        return 1
    say()
    say("[third] done.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
