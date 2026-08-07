#!/usr/bin/env python3
"""Choose the lrmame driver subset: which `SOURCES=` files a shippable build has.

`SOURCES=` is a driver-FILE filter, so every decision here is per file even when
the reason is per title. The output is one path per line, relative to
`src/mame`, ready to be joined with commas and handed to `tools/build-lrmame.sh`.

Inputs (all from `tools/lrmame-driver-index.py`, the device, and the tree):

  --index      JSON from lrmame-driver-index.py
  --mra        MRA setnames from /media/fat/_Arcade (see coverage-diff.py)
  --mame4all   `mame "*" -sourcefile` from the deployed 0.37b5 build

The filters, in order, each reported with what it dropped:

1. **Not working.** `MACHINE_NOT_WORKING`, `MACHINE_MECHANICAL`,
   `MACHINE_IS_SKELETON`, `MACHINE_IS_BIOS_ROOT`. A family with no working
   parent left is not a shippable family. This is the single largest cut and it
   is MAME's own judgement, not ours.
2. **Covered by a MiSTer FPGA core** -- any member has an MRA -- plus
   `coverage-diff.py`'s reviewed exclusion lists: NeoGeo, the console-core false
   gaps, and the PS1-class 3D families.

   **mame4all having a family is NOT a reason to exclude it** (operator rule,
   2026-08-07): the engine ladder is mame4all 0.37b5 < 2003-plus 0.78 <
   lrmame 0.289, and the newest engine that holds real time wins. mame4all is the
   FALLBACK for a family lrmame cannot run at 60 Hz, which is a fact about a
   measurement that has not been taken yet -- so those families have to be IN the
   build for the comparison to be possible at all. They are labelled in the kept
   table instead, and `--skip-mame4all` restores the old behaviour. The `mame "*"
   -sourcefile` input stays required because that label is what says which
   families have a fallback if lrmame comes up short.
3. **Gambling and mahjong.** Fruit machines, poker and pachislot are ~40% of what
   0.289 adds over 0.37b5 and none of it is why this port exists. Two rules, both
   printed in full so they can be argued with: a gambling-manufacturer vendor
   directory, or a majority of the family's working parents matching a title
   marker. `--keep` overrides per family.
4. **DRC policy.** `drcbearm32` is validated on SH-2 and nothing else (Stage 11:
   77/77 differential cases, frame-hash-identical on `psikyosh` hardware), so
   SH-2/SH-3 families are IN and families needing any other DRC CPU -- MIPS,
   PowerPC, E1-32, SHARC -- are OUT. Those run `drcbec` at interpreter speed and
   are 3D-era boards regardless. This inverts `lrmame-drc-scan.sh`'s blanket
   exclusion, which predates the back-end existing.
5. **Parent closure.** A clone whose parent lives in another file produces
   `Driver is a clone of nonexistent driver` and is unrunnable (30 of them in the
   gate build). Any file holding a retained set's parent is pulled in, iterated
   to a fixed point, and reported separately -- those files are build cost with
   no coverage value, so their number is worth seeing.
"""

import argparse
import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
_cov = __import__("importlib").import_module("importlib.util")
_spec = _cov.spec_from_file_location(
    "coverage_diff",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "coverage-diff.py"))
coverage_diff = _cov.module_from_spec(_spec)
_spec.loader.exec_module(coverage_diff)

UNSHIPPABLE_FLAGS = ("MACHINE_NOT_WORKING", "MACHINE_MECHANICAL",
                     "MACHINE_IS_SKELETON", "MACHINE_IS_BIOS_ROOT")

# Manufacturers whose entire output in MAME is gambling hardware. Vendor
# directory is 0.289's own grouping, so this is a list of companies rather than a
# guess about titles.
GAMBLING_VENDORS = {
    "barcrest", "bfm", "jpm", "maygay", "aristocrat", "igt", "amcoe",
    "funworld", "merit", "subsino", "astrocorp", "bmc", "bordun", "adp",
    "wing", "sunwise", "novomatic",
}

# Title markers, applied per family by majority of working parents. Deliberately
# not applied per title: the unit of a build is a file, and `seta/ssv.cpp` holds
# both Super Real Mahjong and Change Air Blade.
GAMBLING_MARKERS = ("poker", "slot machine", "casino", "bingo", "blackjack",
                    "roulette", "keno", "fruit machine", "jackpot", "lottery",
                    "cherry master", "player's edge", "pachislot", "pachinko",
                    "baccarat", "bonus card", "draw poker")

# Gambling manufacturers whose hardware is filed under a NON-gambling vendor
# directory, so the directory rule above cannot see them. `acorn/aristmk5.cpp` is
# the case that produced this list: 42 Aristocrat pokies under `acorn/` because
# the board is ARM-based. Matched on the company field of the working parents,
# majority rule, same as the title markers.
GAMBLING_COMPANIES = ("aristocrat", "igrosoft", "high video", "impera",
                      "status games", "dyna", "amatic", "astro corp",
                      "novomatic", "video klein", "cadillac jack", "merkur")

# Gambling families the two rules above cannot see: the manufacturer also made
# arcade hardware, or the titles are Chinese/Korean gambling whose English names
# carry no marker word. Reviewed by hand against the report, which is why they
# are listed rather than pattern-matched.
GAMBLING_FAMILIES = {
    "igs/goldstar.cpp",      # Cherry Master and the Dyna/IGS poker family
    "igs/igspoker.cpp",
    "igs/igs_m027.cpp",      # Chaoji Dou Dizhu and other IGS card games
    "misc/goldnpkr.cpp",     # Golden Poker / Draw Poker derivatives
    "misc/gei.cpp",          # Greyhound Electronics bar-top poker and trivia
    "misc/coinmstr.cpp",
    "misc/magicard.cpp",
    "misc/umipoker.cpp",
    "misc/multfish.cpp",     # Igrosoft fruit machines
    "misc/highvdeo.cpp",
    "misc/norautp.cpp",
    "misc/calomega.cpp",
}

# `coverage-diff.py` owns the mahjong marker list; reused rather than restated.
MAHJONG_MARKERS = coverage_diff.MAHJONG_MARKERS

# The DRC CPU modules, by whether drcbearm32 has been validated on them.
# Regenerate the underlying set with tools/lrmame-drc-scan.sh's grep.
DRC_CPU_OK = ("sh",)
DRC_CPU_UNPROVEN = ("mips", "powerpc", "e132xs", "unsp", "sharc", "dsp16",
                    "mb86235", "dspp")

INCLUDE_RE = re.compile(r'#include "cpu/([a-z0-9_]+)/([a-z0-9_]+)\.h"')

# CPU headers that place a board in a class this device cannot run at any
# framerate, independent of the DRC question. `TOO_HEAVY_FAMILIES` in
# coverage-diff.py names 0.78-era files by hand; 0.289 adds a dozen more
# (`taitogn`, `ksys573`, `dc_atomiswave`, `segasp`, `konamigs`) and naming them
# one at a time does not scale. The CPU is the honest signal: a PlayStation or a
# Dreamcast is a PlayStation or a Dreamcast whatever the driver file is called.
TOO_HEAVY_CPUS = {
    "psx": "PS1-class (R3000 + GPU)",
    "sh4": "Dreamcast/Naomi-class (SH-4)",
}


LOCAL_INCLUDE_RE = re.compile(r'#include "([a-z0-9_./]+\.h)"')


def cpu_modules(src_root, family_raw, depth=1):
    """-> {(directory, header)} for every `#include "cpu/dir/header.h"`.

    Both halves of the .cpp/.h pair are read: `psikyosh` declares its SH-2 in the
    header, which is the case `lrmame-drc-scan.sh` was written around.

    Local headers are then followed one level, because a shared board header is
    where the heavyweight CPUs hide: `sony/taitogn.cpp` names no CPU at all and
    gets its R3000 through `zn.h`, and `sega/dc_atomiswave.cpp` gets its SH-4
    through `dc.h`. Scanning only the driver pair passed both of those as
    A9-runnable arcade boards.
    """
    cpus = set()
    seen = set()
    frontier = [family_raw, family_raw[:-4] + ".h"]
    for _ in range(depth + 1):
        nxt = []
        for rel in frontier:
            if rel in seen:
                continue
            seen.add(rel)
            path = os.path.join(src_root, rel)
            try:
                with open(path, encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
            except OSError:
                continue
            cpus.update(INCLUDE_RE.findall(text))
            for inc in LOCAL_INCLUDE_RE.findall(text):
                if inc.startswith(("cpu/", "sound/", "video/", "machine/",
                                   "bus/", "emu", "screen", "speaker")):
                    continue
                nxt.append(inc)
                nxt.append(os.path.join(os.path.dirname(rel), inc))
        frontier = nxt
    return cpus


def too_heavy_cpu(cpus):
    """-> the reason string if this board is out of class, else ""."""
    for directory, header in cpus:
        for key, reason in TOO_HEAVY_CPUS.items():
            if directory == key or header.startswith(key):
                return reason
    return ""


def drc_class(cpus):
    """-> "ok" (SH only), "unproven" (any other DRC CPU), or "" (no DRC at all)."""
    dirs = {d for d, _ in cpus}
    if dirs & set(DRC_CPU_UNPROVEN):
        return "unproven"
    if dirs & set(DRC_CPU_OK):
        return "ok"
    return ""


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--index", required=True)
    ap.add_argument("--mra", required=True)
    ap.add_argument("--mame4all", required=True)
    ap.add_argument("--src", default=os.environ.get(
        "LRMAME_SRC", os.path.join(os.path.dirname(here), "vendor", "lrmame")))
    ap.add_argument("--skip-mame4all", action="store_true",
                    help="drop families mame4all already runs. OFF by default: "
                         "the newest engine that holds real time wins, so a "
                         "family mame4all has still needs to be in the lrmame "
                         "build for the two to be compared on it")
    ap.add_argument("--keep", action="append", default=[],
                    help="family path to retain regardless of the gambling rule "
                         "(repeatable)")
    ap.add_argument("--sources", help="write the SOURCES list here, one path per line")
    ap.add_argument("--sources-prefix", default="src/mame/",
                    help="prefix for emitted paths. MAME's SOURCES= is relative "
                         "to the build root, mame.lst is relative to src/mame, "
                         "and the difference is a build that filters to nothing "
                         "(default: src/mame/)")
    ap.add_argument("--extra", action="append", default=[],
                    help="driver file to add regardless of coverage, e.g. a bench "
                         "control the subset would otherwise exclude (repeatable)")
    ap.add_argument("--report", help="write the markdown report here (default stdout)")
    args = ap.parse_args()

    src_root = os.path.join(args.src, "src", "mame")
    games = coverage_diff.parse_index(args.index)
    mra = coverage_diff.parse_mra(args.mra)
    m4a_sets, _ = coverage_diff.parse_mame4all(args.mame4all, "sourcefile")
    keep = set(args.keep)

    by_family = collections.defaultdict(list)
    for g in games:
        by_family[g["family_raw"]].append(g)

    family_of = {g["name"]: g["family_raw"] for g in games}

    # A BIOS root is a parent that is not a game. Whole systems hang off one --
    # `skns` under `suprnova.cpp`, `konamigx` under `konamigx.cpp` -- so every
    # real title in those files has a non-empty `cloneof` and a naive
    # "parent == cloneof is empty" test reports the family as having ZERO
    # shippable games and drops it. Both of those are named gap targets, and both
    # disappeared exactly this way before the rule was fixed.
    bios_roots = {g["name"] for g in games
                  if "MACHINE_IS_BIOS_ROOT" in g["flags"]}

    def is_parent(game):
        return not game["cloneof"] or game["cloneof"] in bios_roots

    dropped = collections.defaultdict(list)   # reason -> [(family, parents)]
    kept = {}

    for family_raw, members in sorted(by_family.items()):
        family = coverage_diff.norm_family(family_raw)
        vendor = family_raw.split("/")[0]
        working = [g for g in members
                   if not any(f in g["flags"] for f in UNSHIPPABLE_FLAGS)]
        parents = [g for g in working if is_parent(g)]
        entry = (family_raw, len(parents),
                 parents[0]["description"] if parents else "")

        if not parents:
            dropped["not working"].append(entry)
            continue
        if family in coverage_diff.HAND_EXCLUDED:
            dropped["MiSTer core (hand-excluded)"].append(entry)
            continue
        if any(g["name"] in mra for g in members):
            dropped["MiSTer core (MRA)"].append(entry)
            continue
        in_mame4all = any(g["name"] in m4a_sets for g in members)
        if in_mame4all and args.skip_mame4all:
            dropped["mame4all already has it"].append(entry)
            continue
        if family in coverage_diff.FALSE_GAP_FAMILIES:
            dropped["console-core false gap"].append(entry)
            continue
        if family in coverage_diff.TOO_HEAVY_FAMILIES:
            dropped["PS1-class 3D"].append(entry)
            continue

        if family_raw not in keep:
            if vendor in GAMBLING_VENDORS or family_raw in GAMBLING_FAMILIES:
                dropped["gambling (manufacturer)"].append(entry)
                continue
            if os.path.basename(family_raw).startswith("nbmj"):
                dropped["mahjong"].append(entry)
                continue
            descs = [(g["description"] or "").lower() for g in parents]
            firms = [(g["company"] or "").lower() for g in parents]
            gambling = sum(1 for d in descs
                           if any(m in d for m in GAMBLING_MARKERS))
            by_firm = sum(1 for f in firms
                          if any(m in f for m in GAMBLING_COMPANIES))
            if by_firm * 2 >= len(firms):
                dropped["gambling (manufacturer)"].append(entry)
                continue
            mahjong = sum(1 for d in descs
                          if any(m in d for m in MAHJONG_MARKERS))
            if gambling * 2 >= len(descs):
                dropped["gambling (titles)"].append(entry)
                continue
            if mahjong * 2 >= len(descs):
                dropped["mahjong"].append(entry)
                continue

        cpus = cpu_modules(src_root, family_raw)
        heavy = too_heavy_cpu(cpus)
        if heavy:
            dropped[f"out of class — {heavy}"].append(entry)
            continue
        cls = drc_class(cpus)
        if cls == "unproven":
            dropped["DRC CPU drcbearm32 has not been validated on"].append(entry)
            continue

        kept[family_raw] = {"parents": len(parents), "drc": cls,
                            "mame4all": in_mame4all,
                            "example": parents[0]["description"]}

    # Parent closure. A retained clone whose parent lives elsewhere is
    # unrunnable, so pull the parent's file in; that file's own clones can then
    # reference further files, hence the fixed point.
    closure = {}
    frontier = set(kept)
    while frontier:
        need = set()
        for family_raw in frontier:
            for g in by_family[family_raw]:
                parent = g["cloneof"]
                if not parent or parent in bios_roots:
                    continue
                home = family_of.get(parent)
                if home and home != family_raw and home not in kept \
                        and home not in closure:
                    need.add(home)
        for family_raw in need:
            parents = [g for g in by_family[family_raw] if is_parent(g)]
            closure[family_raw] = {
                "parents": len(parents),
                "drc": drc_class(cpu_modules(src_root, family_raw)),
                "example": parents[0]["description"] if parents else "(clones only)"}
        frontier = need

    extra = {f for f in args.extra if f not in kept and f not in closure}
    subset = sorted(set(kept) | set(closure) | extra)

    if args.sources:
        with open(args.sources, "w") as fh:
            fh.write("\n".join(args.sources_prefix + f for f in subset) + "\n")

    out = []
    add = out.append
    total_parents = sum(v["parents"] for v in kept.values())
    add("# lrmame driver subset\n")
    add("Generated by `tools/lrmame-subset.py`. Each number is a count of driver")
    add("*files* except where it says parents.\n")
    add(f"- **{len(subset)} driver files** = {len(kept)} carrying coverage "
        f"+ {len(closure)} pulled in only to close clone→parent links"
        + (f" + {len(extra)} added with `--extra` ({', '.join(sorted(extra))})"
           if extra else ""))
    add(f"- **{total_parents} parent romsets** of coverage value")
    add(f"- of the {len(kept)} coverage files, "
        f"**{sum(1 for v in kept.values() if v['drc'] == 'ok')}** need "
        "`drcbearm32` (SH-2/SH-3)")
    add(f"- **{sum(1 for v in kept.values() if v.get('mame4all'))}** of them have a "
        "mame4all fallback if lrmame cannot hold 60 Hz; the other "
        f"**{sum(1 for v in kept.values() if not v.get('mame4all'))}** do not\n")
    add("## Dropped\n")
    add("| reason | files | parents |")
    add("|---|---:|---:|")
    for reason in sorted(dropped, key=lambda r: -len(dropped[r])):
        entries = dropped[reason]
        add(f"| {reason} | {len(entries)} | {sum(e[1] for e in entries)} |")
    add("")
    # Every judgement call is printed in full; only the three mechanical buckets
    # (not working, already covered by a core or by mame4all) are left as counts.
    mechanical = ("not working", "MiSTer core (MRA)", "MiSTer core (hand-excluded)",
                  "mame4all already has it")
    for reason in sorted(dropped, key=lambda r: -sum(e[1] for e in dropped[r])):
        if reason in mechanical:
            continue
        entries = sorted(dropped.get(reason, []), key=lambda e: -e[1])
        if not entries:
            continue
        add(f"### Dropped — {reason} ({len(entries)} files, "
            f"{sum(e[1] for e in entries)} parents)\n")
        add("Printed in full: this is a judgement call, and `--keep <file>` "
            "overrides it.\n")
        add("| file | parents | example |")
        add("|---|---:|---|")
        for family_raw, count, example in entries:
            add(f"| `{family_raw}` | {count} | {example} |")
        add("")
    add(f"## Kept — {len(kept)} files, {total_parents} parents\n")
    add("`fallback` marks a family mame4all also runs: if lrmame cannot hold 60 Hz")
    add("there, the game ships on the older engine instead of not shipping. A blank")
    add("means lrmame is the only engine that has it, so its fps number is the whole")
    add("decision.\n")
    add("| file | parents | DRC | fallback | example |")
    add("|---|---:|---|---|---|")
    for family_raw, v in sorted(kept.items(), key=lambda kv: -kv[1]["parents"]):
        add(f"| `{family_raw}` | {v['parents']} | {v['drc'] or '—'} | "
            f"{'mame4all' if v.get('mame4all') else '—'} | {v['example']} |")
    add("")
    if closure:
        add(f"## Pulled in by parent closure — {len(closure)} files\n")
        add("No coverage value on their own: they hold the parent of a clone in a "
            "kept file.\n")
        add("| file | parents | example |")
        add("|---|---:|---|")
        for family_raw, v in sorted(closure.items()):
            add(f"| `{family_raw}` | {v['parents']} | {v['example']} |")
        add("")

    text = "\n".join(out)
    if args.report:
        with open(args.report, "w") as fh:
            fh.write(text)
        print(f"{len(subset)} files ({len(kept)} + {len(closure)} closure), "
              f"{total_parents} parents", file=sys.stderr)
    else:
        print(text)


if __name__ == "__main__":
    main()
