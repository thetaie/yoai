#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# The external checkouts this repo builds against: clone them, report on them,
# or move them forward. manifest.txt is the record; this reads it.
#
#   ./sync.sh [check|clone|update|verify] [name ...]
#
#     check    (default) say what is there, whether it is on the pinned
#              commit, whether the patches are applied, whether anything else
#              is in the tree. Changes nothing.
#     clone    clone whatever is missing at its pinned commit, init
#              submodules, apply patches. Existing checkouts are left alone.
#     update   move each checkout to its branch tip and rewrite manifest.txt
#              with the new pins. Patches come off first and go back on after,
#              so one that no longer fits STOPS that dependency rather than
#              leaving it half-moved.
#     verify   for the build scripts: silent and exit 0 when each checkout is on
#              its pinned commit with every patch applied; otherwise one line
#              per problem on stderr and exit 1. Changes nothing. A missing
#              checkout counts as a problem. Stray files do not -- a patched
#              tree always has some, so they are check's business, not this.
#
#   name ...   limit to these (manifest names, case-insensitive). No name
#              means all of them.
#
# Checkouts are siblings of this repo, not children of it. That is not taste:
# they are multi-gigabyte trees carrying their own build output, and one
# `git clean -xdf` in here would delete them. Each one's location can be
# overridden by the same environment variable the build scripts already read
# -- LLAMA_SRC, MNN_SRC, QTTS_SRC, VCPKG_ROOT -- so this script and the builds
# cannot end up pointing at different trees.
#
# Env overrides:
#   THIRD_ROOT   where checkouts live (default: this repo's parent directory)
#   <dep>_SRC    one checkout's exact path; see the env column in manifest.txt
#   Each <dep>_SRC also reads as YOAI_<dep>_SRC, from the environment or from
#   .env at the repository root -- the same loader the build scripts call.
#
# This is the twin of sync.cmd. Keep the two in step.
# ---------------------------------------------------------------------------
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Before any default below, so a path set in .env is the one cloned into.
. "$HERE/../scripts/load-env.sh"
# No names: a clone target does not exist yet, and a missing checkout is what
# check and verify report.
yoai_load_env

MANIFEST="$HERE/manifest.txt"
PATCHDIR="$HERE/patches"
THIRD_ROOT="${THIRD_ROOT:-$(cd "$HERE/../.." && pwd)}"

[ -f "$MANIFEST" ] || { echo "no manifest at $MANIFEST" >&2; exit 1; }
command -v git >/dev/null || { echo "need git on PATH" >&2; exit 1; }

# --- Arguments, position-independent ---------------------------------------
# One mode keyword plus any number of dependency names. An unrecognized token
# is an error, not a guess: quietly doing `check` because somebody typed
# `updte` is the failure worth designing out.
MODE=check
ONLY=""
bad=0
for tok in "$@"; do
    case "$tok" in
        check|clone|update|verify) MODE="$tok" ;;
        *)
            if grep -qi "^${tok}|" "$MANIFEST"; then
                ONLY="${ONLY},${tok}"
            else
                echo "unknown argument: $tok" >&2
                bad=1
            fi
            ;;
    esac
done
if [ "$bad" -ne 0 ]; then
    echo "  modes: check | clone | update | verify" >&2
    echo "  names: the first column of manifest.txt" >&2
    exit 1
fi
[ -n "$ONLY" ] && ONLY="${ONLY},"

if [ "$MODE" != verify ]; then
    echo "[third] root=$THIRD_ROOT  mode=$MODE"
    echo
fi

failed=""

# --- Patches ---------------------------------------------------------------
# Every patch for one dependency, in manifest order. $1 is apply or reverse.
#
# `--reverse --check` is the idempotence test, and a better one than a marker
# file: it succeeds only when the change is ALREADY present. So applying twice
# is a no-op, a checkout that moved under a patch fails loudly, and a file
# somebody edited by hand is detected rather than patched on top of.
#
# The loop stops at the first failure: half-patching a tree is worse than not
# touching it.
do_patches() {
    local act="$1" spec file sub tree
    [ "$D_PATCHES" = "-" ] && return 0
    local IFS=,
    for spec in $D_PATCHES; do
        file="${spec%%@*}"
        sub="${spec#*@}"
        tree="$DIR"
        [ "$sub" != "." ] && tree="$DIR/$sub"
        if [ ! -f "$PATCHDIR/$file" ]; then
            echo "    no patch at $PATCHDIR/$file" >&2
            return 1
        fi
        if git -C "$tree" apply --reverse --check "$PATCHDIR/$file" >/dev/null 2>&1; then
            if [ "$act" = apply ]; then
                echo "    patch ok: $file"
            else
                git -C "$tree" apply --reverse "$PATCHDIR/$file" || {
                    echo "    could not reverse $file in $tree" >&2; return 1; }
                echo "    un-patched: $file"
            fi
        elif [ "$act" = apply ]; then
            git -C "$tree" apply "$PATCHDIR/$file" || {
                echo "    COULD NOT APPLY $file in $tree -- rebase it against the new tree." >&2
                return 1; }
            echo "    patched: $file"
        fi
    done
    return 0
}

# --- Clone -----------------------------------------------------------------
# At the pinned commit. Not --depth 1 and not --branch: the pin is a commit
# rather than a tag, and it is usually behind the tip, so neither shortcut can
# check it out.
do_clone() {
    echo "    cloning..."
    git clone "$D_URL" "$DIR" || return 1
    git -C "$DIR" checkout --detach "$D_COMMIT" || return 1
    if [ "$D_SUB" = yes ]; then
        git -C "$DIR" submodule update --init --recursive || return 1
    fi
    do_patches apply
}

# --- Update ----------------------------------------------------------------
# Move to the branch tip. The patches come off before the fetch: pulling over a
# patched file either conflicts or silently merges, and neither is a thing to
# discover during a build three days later.
do_update() {
    do_patches reverse || return 1
    echo "    fetching..."
    git -C "$DIR" fetch origin "$D_BRANCH" || return 1
    git -C "$DIR" checkout --detach FETCH_HEAD || return 1
    if [ "$D_SUB" = yes ]; then
        git -C "$DIR" submodule update --init --recursive || return 1
    fi
    D_COMMIT="$(git -C "$DIR" rev-parse HEAD)"
    do_patches apply
}

# --- Report ----------------------------------------------------------------
# What is there, against what the manifest says. Three facts reported
# separately because they go wrong separately: the commit, the patches, and
# whatever else somebody left in the tree.
do_report() {
    local head spec file sub tree ndirty
    head="$(git -C "$DIR" rev-parse HEAD)"
    if [ "$head" = "$D_COMMIT" ]; then
        echo "    commit ${head:0:12} (pinned)"
    else
        echo "    commit ${head:0:12} -- manifest pins ${D_COMMIT:0:12}"
    fi
    if [ "$D_PATCHES" != "-" ]; then
        local IFS=,
        for spec in $D_PATCHES; do
            file="${spec%%@*}"; sub="${spec#*@}"; tree="$DIR"
            [ "$sub" != "." ] && tree="$DIR/$sub"
            if git -C "$tree" apply --reverse --check "$PATCHDIR/$file" >/dev/null 2>&1; then
                echo "    patch ok: $file"
            else
                echo "    patch MISSING: $file"
            fi
        done
    fi
    # Anything beyond the patches. A patched file is EXPECTED to show as
    # modified, so this counts paths instead of asserting a clean tree: it is a
    # prompt to look, not a failure.
    ndirty="$(git -C "$DIR" status --porcelain | wc -l | tr -d ' ')"
    [ "$ndirty" -gt 0 ] && echo "    $ndirty modified/untracked path(s) -- git -C \"$DIR\" status"
    return 0
}

# --- Verify ----------------------------------------------------------------
# The same two facts do_report states, said only when they are wrong, so a
# build can run this every time and print nothing on the normal path.
do_verify() {
    local head spec file sub tree bad=0
    head="$(git -C "$DIR" rev-parse HEAD)"
    if [ "$head" != "$D_COMMIT" ]; then
        echo "[third] $D_NAME: commit ${head:0:12}, manifest pins ${D_COMMIT:0:12}" >&2
        bad=1
    fi
    if [ "$D_PATCHES" != "-" ]; then
        local IFS=,
        for spec in $D_PATCHES; do
            file="${spec%%@*}"; sub="${spec#*@}"; tree="$DIR"
            [ "$sub" != "." ] && tree="$DIR/$sub"
            if ! git -C "$tree" apply --reverse --check "$PATCHDIR/$file" >/dev/null 2>&1; then
                echo "[third] $D_NAME: patch MISSING: $file" >&2
                bad=1
            fi
        done
    fi
    return $bad
}

# --- The table -------------------------------------------------------------
# Read once into an array so `update` can re-emit manifest.txt from it at the
# end. Lines starting with # are the header; an empty column is written `-`
# rather than left blank, to keep the two scripts reading the same file the
# same way.
lines=()
while IFS='|' read -r name env url branch commit sub patches usedby; do
    case "$name" in ''|'#'*) continue ;; esac
    # sync.cmd writes CRLF, so the last column arrives with a CR stuck to it.
    # Strip it here and write CRLF back below, so both scripts produce the same
    # bytes and the file does not flip line endings depending on who ran last.
    usedby="${usedby%$'\r'}"
    D_NAME="$name"; D_ENV="$env"; D_URL="$url"; D_BRANCH="$branch"
    D_COMMIT="$commit"; D_SUB="$sub"; D_PATCHES="$patches"; D_USEDBY="$usedby"

    if [ -n "$ONLY" ] && ! printf '%s' "$ONLY" | grep -qi ",${D_NAME},"; then
        lines+=("$D_NAME|$D_ENV|$D_URL|$D_BRANCH|$D_COMMIT|$D_SUB|$D_PATCHES|$D_USEDBY")
        continue
    fi

    # Where it lives: the build scripts' own variable wins, so the two can
    # never disagree about which tree is being built.
    DIR="${!D_ENV:-}"
    [ -z "$DIR" ] && DIR="$THIRD_ROOT/$D_NAME"

    if [ "$MODE" = verify ]; then
        if [ ! -d "$DIR/.git" ]; then
            echo "[third] $D_NAME: MISSING at $DIR" >&2
            failed="$failed $D_NAME"
        elif ! do_verify; then
            failed="$failed $D_NAME"
        fi
        lines+=("$D_NAME|$D_ENV|$D_URL|$D_BRANCH|$D_COMMIT|$D_SUB|$D_PATCHES|$D_USEDBY")
        continue
    fi

    echo "=== $D_NAME  ($D_USEDBY)"
    echo "    $DIR"

    if [ ! -d "$DIR/.git" ]; then
        if [ "$MODE" != clone ]; then
            echo "    MISSING -- \`./sync.sh clone $D_NAME\` would fetch it"
            echo
            lines+=("$D_NAME|$D_ENV|$D_URL|$D_BRANCH|$D_COMMIT|$D_SUB|$D_PATCHES|$D_USEDBY")
            continue
        fi
        if ! do_clone; then
            failed="$failed $D_NAME"; echo
            lines+=("$D_NAME|$D_ENV|$D_URL|$D_BRANCH|$D_COMMIT|$D_SUB|$D_PATCHES|$D_USEDBY")
            continue
        fi
    fi

    if [ "$MODE" = update ] && ! do_update; then
        failed="$failed $D_NAME"; echo
        lines+=("$D_NAME|$D_ENV|$D_URL|$D_BRANCH|$D_COMMIT|$D_SUB|$D_PATCHES|$D_USEDBY")
        continue
    fi

    do_report
    echo
    lines+=("$D_NAME|$D_ENV|$D_URL|$D_BRANCH|$D_COMMIT|$D_SUB|$D_PATCHES|$D_USEDBY")
done < "$MANIFEST"

if [ "${#lines[@]}" -eq 0 ]; then
    echo "manifest.txt has no entries." >&2
    exit 1
fi

# Re-emit manifest.txt, carrying whatever `update` moved. Only the commit
# column ever changes, but re-emitting whole lines keeps one code path instead
# of a search-and-replace that has to know the column layout.
if [ "$MODE" = update ]; then
    {
        printf '# name|env|url|branch|commit|submodules|patches (file@worktree)|used by\r\n'
        printf '%s\r\n' "${lines[@]}"
    } > "$MANIFEST.tmp" && mv "$MANIFEST.tmp" "$MANIFEST"
    echo
    echo "[third] manifest.txt rewritten with the new pins -- git diff third/manifest.txt"
fi

# verify says nothing on success and has already named each problem.
if [ "$MODE" = verify ]; then
    [ -z "$failed" ]
    exit $?
fi

if [ -n "$failed" ]; then
    echo
    echo "[third] trouble with:$failed"
    exit 1
fi
echo
echo "[third] done."
