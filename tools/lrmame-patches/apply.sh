#!/usr/bin/env bash
# Apply this project's patches to vendor/lrmame.
#
#   tools/lrmame-patches/apply.sh            # apply everything not yet applied
#   tools/lrmame-patches/apply.sh --check    # report state, change nothing
#   tools/lrmame-patches/apply.sh --revert   # undo, newest first
#
# These are upstream BUGS, not MAMESTer features -- anything that is a feature
# of this port belongs in tools/mame-frontend/ or tools/mame-drc-arm32/ and is
# injected there. Keep this directory for things that should eventually go
# upstream and disappear from here.
#
# WHY `git apply` AND NOT SENTINEL FENCES. tools/mame-drc-arm32/inject.sh marks
# its edits with a sentinel comment and reverts by deleting whatever the fence
# contains. That has been wrong twice (see docs/superpowers/progress.md -- once
# it swallowed upstream's own cpu.lua lines and left the tree unable to link),
# and both times it was only visible on an alternating build. A patch that
# REPLACES an upstream line cannot be expressed as an additive fence at all,
# which is the case here. `git apply` knows how to reverse its own hunks, and
# `--check` tells us whether a hunk is applicable without touching anything.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${MAME_SRC:-$(cd "$HERE/../../vendor/lrmame" && pwd)}"
MODE="${1:-apply}"

# NOT `[ -d "$SRC/.git" ]`: in a submodule .git is a FILE containing a gitdir:
# pointer, so the directory test fails on exactly the tree this script targets.
git -C "$SRC" rev-parse --git-dir >/dev/null 2>&1 \
    || { echo "not a git tree: $SRC" >&2; exit 1; }

# ONE PATCH PER REGION. state() below tests each patch against the tree as it
# currently stands, which is only meaningful while patches do not overlap: two
# patches touching the same function stack fine on apply, but the second one's
# context then invalidates the first one's reverse-check and --check reports a
# phantom conflict. Fixing that properly means replaying the series against a
# scratch tree; not fixing it means a revert path that misbehaves only in
# certain orders, which this project has already been bitten by twice. So keep
# related edits to one file in one patch and the problem does not arise.
#
# Sorted, so 0001 lands before 0002 when they DO target different files.
mapfile -t PATCHES < <(find "$HERE" -maxdepth 1 -name '*.patch' | sort)
[ "${#PATCHES[@]}" -gt 0 ] || { echo "no patches in $HERE" >&2; exit 0; }

# `git apply -R --check` succeeding means the patch is ALREADY applied; plain
# `--check` succeeding means it is applicable and therefore is not. Testing
# both distinguishes "already done" from "does not fit this tree", and only the
# second is an error worth stopping for.
state() {  # -> applied | pending | conflict
    if git -C "$SRC" apply -R --check "$1" >/dev/null 2>&1; then echo applied
    elif git -C "$SRC" apply    --check "$1" >/dev/null 2>&1; then echo pending
    else echo conflict
    fi
}

rc=0
case "$MODE" in
--check)
    for p in "${PATCHES[@]}"; do
        s=$(state "$p")
        printf '%-10s %s\n' "$s" "$(basename "$p")"
        [ "$s" = conflict ] && rc=1
    done
    exit $rc
    ;;
--revert)
    # Newest first, the reverse of the apply order.
    for (( i=${#PATCHES[@]}-1; i>=0; i-- )); do
        p="${PATCHES[$i]}"
        case "$(state "$p")" in
        applied)  git -C "$SRC" apply -R "$p"; echo "reverted $(basename "$p")" ;;
        pending)  echo "not applied  $(basename "$p")" ;;
        conflict) echo "CONFLICT     $(basename "$p") -- left alone" >&2; rc=1 ;;
        esac
    done
    exit $rc
    ;;
apply)
    for p in "${PATCHES[@]}"; do
        case "$(state "$p")" in
        applied)  echo "already applied $(basename "$p")" ;;
        pending)  git -C "$SRC" apply "$p"; echo "applied $(basename "$p")" ;;
        conflict)
            # Loud and fatal. A patch that no longer fits after a submodule bump
            # means the upstream code moved, and silently building without it
            # would produce a binary that renders wrongly rather than one that
            # fails -- which is exactly how the M16B bug went unnoticed.
            echo "CONFLICT: $(basename "$p") does not apply to $SRC" >&2
            echo "  the submodule probably moved; re-cut the patch" >&2
            exit 1 ;;
        esac
    done
    ;;
*)
    echo "usage: $0 [--check|--revert]" >&2; exit 2 ;;
esac
