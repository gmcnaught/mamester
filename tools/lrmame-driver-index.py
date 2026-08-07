#!/usr/bin/env python3
"""Build a game index for lrmame (MAME 0.289) without building MAME.

`coverage-diff.py` was written against the 2003-plus reference set's `-listinfo`
XML. 0.289 ships no such file and the only way to emit one is `-listxml` from a
built binary -- a full-driver host build, which is hours for metadata we can read
straight out of the tree. This produces the same three fields the coverage diff
actually consumes (setname, source file, parent) from two sources that are
already in the submodule:

  src/mame/mame.lst    which setnames are ENABLED, grouped by `@source:` file.
                       This is the authority on membership: a GAME() macro that
                       mame.lst does not list is not in any build.
  src/mame/**/*.cpp    the GAME()/GAMEL() macros, for parent and description.

Output is JSON: [{name, family, family_raw, cloneof, description, ...}, ...],
consumed by `coverage-diff.py --index`.

WHAT THIS DOES NOT CARRY, and why it is acceptable here:

- **Screen geometry.** Resolution lives in the machine config (`set_raw`,
  `set_size`), not in the registration macro, and parsing it out of C++ is a
  different and much less reliable job than parsing a macro argument list. The
  coverage diff's >512-pixel geometry-risk section is therefore empty when fed an
  index; it is not wrong, it is unmeasured, and the present path already falls
  back at runtime for a frame it cannot scan out natively.
- **`GAME_CUSTOM()`** (5,238 uses) is a per-driver local macro whose setname is
  built by token pasting, so it cannot be read without expanding the file, and
  those setnames are dropped. Measured rather than assumed: exactly **three**
  families lose ALL of their members this way -- `barcrest/mpu4avan.cpp`,
  `mpu4bwb.cpp` and `mpu4concept.cpp`, 1,041 Barcrest fruit-machine sets, none of
  them arcade. Everywhere else (`neogeo`, `cps1` bootlegs, `megatech`) the family
  still has GAME() entries, so it stays visible and the loss is some of its
  clones.
- **CONS/COMP/SYST** entries are computers and consoles, not arcade hardware.
  They are deliberately not parsed, so their setnames fall out of the index and
  the 1,605 families with no GAME() at all disappear from the arcade universe --
  which is the intent. The count is reported on stderr.
"""

import argparse
import json
import os
import re
import sys

MAME_MACROS = ("GAME", "GAMEL")

# GAME( year, name, parent, machine, input, class, init, monitor, company,
#       fullname, flags )                                    -- 11 arguments
# GAMEL( ... , flags, layout )                               -- 12
ARG_NAME, ARG_PARENT, ARG_COMPANY, ARG_FULLNAME, ARG_FLAGS = 1, 2, 8, 9, 10

MACRO_RE = re.compile(r"^[ \t]*(GAME|GAMEL)[ \t]*\(", re.MULTILINE)


def split_args(text, start):
    """Split one macro call's arguments, from the '(' at `start`.

    Depth-tracked rather than line- or comma-split: a MAME registration line
    carries nested parentheses (`ROT270|ORIENTATION_FLIP_X` is fine, but
    `MACHINE_FLAGS(x, y)` and adjacent string literals containing commas are
    not), and roughly a fifth of them wrap onto a second line.

    Returns (args, index just past the closing paren), or (None, start) if the
    call does not close -- which happens inside `#if 0` blocks and comments.
    """
    depth = 0
    arg, args = [], []
    i = start
    n = len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            # Copy the whole literal, escapes included: descriptions contain
            # both commas and parentheses.
            quote = c
            arg.append(c)
            i += 1
            while i < n:
                arg.append(text[i])
                if text[i] == "\\":
                    i += 2
                    if i <= n:
                        arg.append(text[i - 1])
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "(":
            depth += 1
            if depth == 1:
                i += 1
                continue
        elif c == ")":
            depth -= 1
            if depth == 0:
                args.append("".join(arg).strip())
                return args, i + 1
        elif c == "," and depth == 1:
            args.append("".join(arg).strip())
            arg = []
            i += 1
            continue
        arg.append(c)
        i += 1
    return None, start


STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def dequote(field):
    """Concatenated literals -> one string; a bare identifier -> itself.

    Long titles are split across adjacent literals, and a few are a macro
    (`ALT_NAME`), which comes back unchanged rather than empty.
    """
    parts = STRING_RE.findall(field or "")
    if parts:
        return "".join(p.replace('\\"', '"') for p in parts)
    return (field or "").strip()


def parse_mame_lst(path, src_root=None):
    """-> ({setname: family_raw}, ordered family list).

    `family_raw` is the path as mame.lst writes it, relative to src/mame --
    which is exactly the form `SOURCES=` wants, so it is kept verbatim rather
    than reduced to a basename.

    mame.lst is hand-maintained and can name a file that does not exist: 0.289
    lists `cv1k` twice, under both `cave/cv1k.cpp` (real) and `misc/cv1k.cpp`
    (stale). Last-writer-wins would file every Cave CV1000 set under the dead
    path -- which silently loses the SH-3 classification and emits a `SOURCES=`
    entry the build cannot resolve. When `src_root` is given, a block whose file
    is absent is skipped and counted.
    """
    enabled, families = {}, []
    source = None
    skipped_sources = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if line.startswith("@source:"):
                source = line[len("@source:"):].strip()
                if src_root and not os.path.isfile(os.path.join(src_root, source)):
                    skipped_sources.append(source)
                    source = None
                    continue
                families.append(source)
                continue
            if not line or line.startswith("//") or line.startswith("/*") \
                    or line.startswith("*") or line.startswith("#"):
                continue
            if source:
                # Trailing `// comment` after a setname happens in a few places.
                enabled[line.split()[0].lower()] = source
    if skipped_sources:
        print(f"note: {len(skipped_sources)} @source blocks name a file that does "
              f"not exist and were skipped: {', '.join(sorted(skipped_sources))}",
              file=sys.stderr)
    return enabled, families


DEFINE_RE = re.compile(r"^[ \t]*#define[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]+([^\n(].*)$",
                       re.MULTILINE)
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def expand_local_defines(flags, defines, depth=3):
    """Resolve driver-local flag macros against the file's own `#define`s.

    `MACHINE_FLAGS` is defined three times in the tree and used 3,150 times, and
    every definition contains `MACHINE_NOT_WORKING` -- so a consumer that greps
    the raw text for that flag would pass 3,150 non-working sets as shippable.
    Object-like defines only, one file at a time, and no recursion beyond
    `depth`: this resolves the flags idiom, it is not a C preprocessor.
    """
    for _ in range(depth):
        expanded = IDENT_RE.sub(
            lambda m: defines.get(m.group(0), m.group(0)), flags)
        if expanded == flags:
            break
        flags = expanded
    return " ".join(flags.split())


def parse_registrations(src_root, files):
    """-> {setname: {parent, description, company, flags}} from GAME()/GAMEL()."""
    found = {}
    unreadable = 0
    for rel in files:
        path = os.path.join(src_root, rel)
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            unreadable += 1
            continue
        defines = {name: body.split("//")[0].split("/*")[0].strip()
                   for name, body in DEFINE_RE.findall(text)}
        for m in MACRO_RE.finditer(text):
            args, _ = split_args(text, m.end() - 1)
            if not args or len(args) <= ARG_FULLNAME:
                continue
            name = dequote(args[ARG_NAME]).lower()
            if not name:
                continue
            parent = dequote(args[ARG_PARENT]).lower()
            # Flags is a `|`-joined expression, sometimes wrapped over a line and
            # sometimes a driver-local #define. Kept as raw text: the consumer
            # tests for substrings, and a #define it cannot see reads as no flag
            # rather than as the wrong flag.
            flags = expand_local_defines(
                args[ARG_FLAGS] if len(args) > ARG_FLAGS else "", defines)
            found[name] = {
                "parent": "" if parent in ("0", "", "null") else parent,
                "description": dequote(args[ARG_FULLNAME]),
                "company": dequote(args[ARG_COMPANY]),
                "flags": flags,
            }
    if unreadable:
        print(f"note: {unreadable} driver files listed in mame.lst are missing",
              file=sys.stderr)
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--src", default=os.environ.get(
        "LRMAME_SRC", os.path.join(os.path.dirname(here), "vendor", "lrmame")),
        help="lrmame checkout (default vendor/lrmame, or $LRMAME_SRC)")
    ap.add_argument("--out", required=True, help="JSON index to write")
    args = ap.parse_args()

    mamedir = os.path.join(args.src, "src", "mame")
    lst = os.path.join(mamedir, "mame.lst")
    if not os.path.isfile(lst):
        sys.exit(f"no mame.lst under {mamedir} — "
                 "run: git submodule update --init vendor/lrmame")

    enabled, families = parse_mame_lst(lst, mamedir)
    regs = parse_registrations(mamedir, families)

    games, no_macro = [], []
    for name, family_raw in sorted(enabled.items()):
        reg = regs.get(name)
        if reg is None:
            # Either a CONS/COMP/SYST entry (not arcade) or a GAME_CUSTOM one.
            no_macro.append(name)
            continue
        games.append({
            "name": name,
            "family": os.path.basename(family_raw).rsplit(".", 1)[0].lower(),
            "family_raw": family_raw,
            "cloneof": reg["parent"] or None,
            "description": reg["description"],
            "company": reg["company"],
            "flags": reg["flags"],
            "width": None, "height": None, "refresh": None,
            "orientation": None, "status": None,
        })

    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(games, fh, indent=1)

    arcade_families = {g["family_raw"] for g in games}
    print(f"mame.lst: {len(enabled)} enabled setnames across {len(families)} files",
          file=sys.stderr)
    print(f"index:    {len(games)} arcade entries "
          f"({sum(1 for g in games if not g['cloneof'])} parents) "
          f"across {len(arcade_families)} families", file=sys.stderr)
    print(f"dropped:  {len(no_macro)} setnames with no GAME()/GAMEL() macro "
          "(CONS/COMP/SYST computers and consoles, plus GAME_CUSTOM token-paste "
          "sets)", file=sys.stderr)


if __name__ == "__main__":
    main()
