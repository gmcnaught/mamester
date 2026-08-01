#!/usr/bin/env python3
"""Which hardware families would mame2003-plus add that nothing else covers?

Three inputs, all cheap to obtain and none requiring a build:

  --listinfo  the MAME-format XML shipped with the 2003-plus reference romset
              (archive.org/details/mame-2003-plus-reference-set)
  --mame4all  either `mame "*" -sourcefile` output from the shipped build, or a
              bare list of driver source filenames (`ls src/drivers/`). Both are
              accepted; the second needs no device.
  --mra       every <setname> and zip= name under /media/fat/_Arcade, one per line

Judgement is at FAMILY level (the driver source file), per the Stage 8 scope rule:
a family counts as covered if ANY of its drivers has a MiSTer MRA. Per-driver
matching is useless because clone setnames are rarely named in MRAs, which made
cps1/pacman/system16 look like gaps.

Family names are normalised before comparison: mame4all's driver files are .cpp
and 2003-plus's are .c, so `gng.cpp` and `gng.c` are the same family and would
otherwise read as a coverage win that does not exist.
"""

import argparse
import json
import os
import sys
import xml.etree.ElementTree as ET

# NeoGeo has a wholesale MiSTer core but ships .neo files rather than MRAs, so it
# never appears in the MRA setname list and would otherwise read as a huge gap.
HAND_EXCLUDED = {"neogeo"}

# Families that pass the MRA test but that docs/feasibility.md §5 says explicitly
# not to count. Reported as their own sections rather than silently dropped: the
# headline is net of them, but the numbers stay visible and arguable.
#
# "False gaps" run natively and BETTER on a MiSTer console-style core, so software
# MAME is strictly worse for them. They evade MRA matching because those cores do
# not ship MRAs.
FALSE_GAP_FAMILIES = {
    "playch10": "PlayChoice-10 — runs on the NES core",
    "vsnes":    "Vs. System — runs on the NES core",
    "nss":      "Nintendo Super System — runs on the SNES core",
    "segac2":   "Sega C-2 — has its own MiSTer core",
    "stv":      "Sega ST-V — runs on the Saturn core",
}

# PS1-class 3D. feasibility.md §5: "Do not count these as unlocked." Included here
# so the count is explicit rather than an unstated assumption.
TOO_HEAVY_FAMILIES = {
    "namcos11": "PS1-class 3D (Tekken)",
    "namcos12": "PS1-class 3D (Tekken 3)",
    "namcos21": "polygon 3D (Winning Run)",
    "namcos22": "polygon 3D (Alpine Racer)",
    "zn":       "Sony ZN (PS1 arcade)",
    "seattle":  "Midway Seattle 3D",
    "model2":   "Sega Model 2",
    "model3":   "Sega Model 3",
    "cojag":    "Atari Cojag (Jaguar hardware)",
    "vegas":    "Midway Vegas 3D",
    "viper":    "Konami Viper",
    "hornet":   "Konami Hornet 3D",
    "zr107":    "Konami ZR107 3D",
    "gticlub":  "Konami GTI Club 3D",
}

# Mahjong is tagged PER TITLE, not per family: feasibility.md calls it numerically
# large and niche, but the families are mixed. seta2.c holds both Super Real
# Mahjong and Mobile Suit Gundam EX Revue, and ssv.c likewise, so excluding whole
# families on this basis would throw away non-mahjong games.
MAHJONG_MARKERS = ("mahjong", "janshi", "jansou", "jong", "janputer",
                   "ojanko", "niyanpai", "pai ", "reach ")

# The DDR reader's line FIFO tops out at 512 pixels; wider frames cannot be
# presented natively and fall back (docs/superpowers/progress.md, Stage 3).
MAX_NATIVE_WIDTH = 512


def norm_family(name):
    """`src/drivers/gng.cpp` and `gng.c` are the same family."""
    if not name:
        return ""
    base = os.path.basename(name.strip())
    for ext in (".cpp", ".c"):
        if base.endswith(ext):
            base = base[: -len(ext)]
            break
    return base.lower()


def parse_listinfo(path):
    """-> list of dicts, one per game entry."""
    games = []
    # iterparse keeps peak memory sane on the 20 MB document.
    for _event, elem in ET.iterparse(path, events=("end",)):
        if elem.tag != "game":
            continue
        video = elem.find("video")
        desc = elem.find("description")
        driver = elem.find("driver")

        def vattr(key):
            return video.get(key) if video is not None else None

        games.append({
            "name": elem.get("name"),
            "family": norm_family(elem.get("sourcefile")),
            "family_raw": elem.get("sourcefile") or "",
            "cloneof": elem.get("cloneof"),
            "description": desc.text if desc is not None else "",
            "width": int(vattr("width")) if vattr("width") else None,
            "height": int(vattr("height")) if vattr("height") else None,
            "refresh": float(vattr("refresh")) if vattr("refresh") else None,
            "orientation": vattr("orientation"),
            "status": driver.get("status") if driver is not None else None,
        })
        elem.clear()
    return games


def parse_mame4all(path, kind):
    """What mame4all already has.

    `kind` is explicit rather than sniffed. Two of the three accepted inputs are
    single-column and mean opposite things -- a list of setnames and a list of
    driver filenames look identical -- and guessing between them silently
    produced a wrong answer once already.

      "sourcefile" -- `mame "*" -sourcefile`: `setname<whitespace>source.c`
      "setnames"   -- one setname per line
      "families"   -- one driver source filename per line (`ls src/drivers/`)

    Returns (setnames, families); the unavailable half is empty.
    """
    setnames, families = set(), set()
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            parts = line.split()
            if not parts:
                continue
            if kind == "sourcefile":
                if len(parts) < 2:
                    continue
                setnames.add(parts[0])
                families.add(norm_family(parts[-1]))
            elif kind == "setnames":
                setnames.add(parts[0])
            else:
                families.add(norm_family(parts[0]))
    return setnames, families


def parse_mra(path):
    """Setnames covered by a MiSTer MRA.

    Lines carrying a path prefix (28 `hbmame/...`, 3 `sound/...` in the current
    list) are skipped rather than basenamed. Verified no-op: basenaming would add
    only `galnamco` and `pacupacu1`, neither a 0.78 setname, while every other
    prefixed name already appears unprefixed. Skipping also avoids merging an
    hbmame ROM hack with its mainline namesake.
    """
    covered, skipped = set(), 0
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            name = line.strip()
            if not name:
                continue
            if "/" in name:
                skipped += 1
                continue
            covered.add(name)
    if skipped:
        print(f"note: skipped {skipped} path-prefixed MRA entries", file=sys.stderr)
    return covered


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listinfo", required=True)
    ap.add_argument("--mame4all", required=True)
    ap.add_argument("--mame4all-kind", required=True,
                    choices=("sourcefile", "setnames", "families"),
                    help="format of --mame4all. Never sniffed: a setname list and "
                         "a driver-filename list are both single-column and mean "
                         "opposite things.")
    ap.add_argument("--mra", required=True)
    ap.add_argument("--json")
    ap.add_argument("--top", type=int, default=60,
                    help="how many families to table in the report (default 60)")
    args = ap.parse_args()

    games = parse_listinfo(args.listinfo)
    m4a_sets, m4a_families = parse_mame4all(args.mame4all, args.mame4all_kind)
    mra_sets = parse_mra(args.mra)

    if not m4a_sets:
        print("WARNING: no setnames supplied, so matching falls back to driver "
              "FILENAMES. Those are renamed between MAME versions "
              "(mame4all's wmsyunit.cpp is 2003-plus's midyunit.c), so the "
              "new-family count will OVERSTATE. Prefer --mame4all-kind setnames.",
              file=sys.stderr)

    by_family = {}
    for g in games:
        by_family.setdefault(g["family"], []).append(g)

    new_families, improved, false_gaps, too_heavy = [], [], [], []
    covered_count = 0

    for family, members in sorted(by_family.items()):
        if family in HAND_EXCLUDED:
            continue
        if any(g["name"] in mra_sets for g in members):
            covered_count += 1
            continue

        parents = sorted(g["name"] for g in members if not g["cloneof"])
        if not parents:
            # Every member is a clone of a parent living in another family.
            continue
        entry = {
            "family": members[0]["family_raw"] or family,
            "parents": parents,
            "count": len(parents),
            "example": next((g["description"] for g in members if not g["cloneof"]), ""),
        }

        # Does mame4all already have this hardware?
        #
        # Match on SETNAMES, not family filenames. Driver files get renamed
        # between MAME versions -- mame4all calls the Midway hardware
        # wmsyunit.cpp/wmstunit.cpp where 2003-plus calls it midyunit.c/midtunit.c
        # -- so filename matching reports renamed families as coverage wins that
        # do not exist. Setnames are stable across versions; that is what a
        # romset IS.
        if m4a_sets:
            in_mame4all = any(g["name"] in m4a_sets for g in members)
        else:
            # Degraded mode: only a family listing was supplied, so fall back to
            # filename matching and warn, because the answer will overstate.
            in_mame4all = family in m4a_families

        entry["mahjong_parents"] = sum(
            1 for g in members
            if not g["cloneof"]
            and any(m in (g["description"] or "").lower() for m in MAHJONG_MARKERS))

        if in_mame4all:
            # Reachable today via mame4all; 2003-plus may emulate it better, but
            # it is not a coverage win.
            improved.append(entry)
        elif family in FALSE_GAP_FAMILIES:
            entry["reason"] = FALSE_GAP_FAMILIES[family]
            false_gaps.append(entry)
        elif family in TOO_HEAVY_FAMILIES:
            entry["reason"] = TOO_HEAVY_FAMILIES[family]
            too_heavy.append(entry)
        else:
            new_families.append(entry)

    geometry_risk = [
        {"name": g["name"], "family": g["family_raw"],
         "width": g["width"], "height": g["height"]}
        for g in games if g["width"] and g["width"] > MAX_NATIVE_WIDTH
    ]

    for bucket in (new_families, improved, false_gaps, too_heavy):
        bucket.sort(key=lambda e: (-e["count"], e["family"]))
    total_new_parents = sum(e["count"] for e in new_families)
    mahjong_parents = sum(e["mahjong_parents"] for e in new_families)

    out = []
    add = out.append
    add("# mame2003-plus coverage diff\n")
    add("Generated by `tools/coverage-diff.py`. Family-level judgement per the")
    add("Stage 8 scope rule; NeoGeo excluded by hand (wholesale core, ships `.neo`,")
    add("never appears in the MRA list).\n")
    add(f"- 2003-plus game entries: **{len(games)}**, "
        f"parents: **{sum(1 for g in games if not g['cloneof'])}**")
    add(f"- 2003-plus hardware families: **{len(by_family)}**")
    if m4a_sets:
        add(f"- mame4all setnames: **{len(m4a_sets)}** (the matching basis)")
    else:
        add(f"- mame4all families: **{len(m4a_families)}** "
            "(filename matching — overstates, see warning)")
    add(f"- MiSTer MRA setnames: **{len(mra_sets)}**")
    add(f"- families already covered by a MiSTer core: **{covered_count}**\n")
    add("Matching against mame4all is on **setnames, not driver filenames**. "
        "Driver files")
    add("get renamed between MAME versions — mame4all's `wmsyunit.cpp` is "
        "2003-plus's")
    add("`midyunit.c`, the same Midway hardware Stage 8 benched `mk` and `nbajam` "
        "on —")
    add("so filename matching reports renamed families as coverage wins that do "
        "not exist.\n")
    add(f"## New: {len(new_families)} families, {total_new_parents} parent romsets\n")
    add("Families 2003-plus has that mame4all does not, that no MiSTer core "
        "covers, and")
    add("that `docs/feasibility.md` §5 does not rule out. **This is the coverage "
        "win**, net")
    add("of the two excluded categories below.\n")
    if mahjong_parents:
        add(f"Of those {total_new_parents} parents, **{mahjong_parents}** are "
            f"mahjong titles by description — numerically large, niche value, and")
        add("many are adult titles. Tagged per title rather than per family, "
            "because the")
        add("families are mixed (`seta2.c` holds both Super Real Mahjong and "
            "Mobile Suit")
        add(f"Gundam EX Revue). Net of mahjong: "
            f"**{total_new_parents - mahjong_parents} parents**.\n")
    add("| family | parents | example |")
    add("|---|---:|---|")
    for e in new_families[:args.top]:
        add(f"| `{e['family']}` | {e['count']} | {e['example']} |")
    if len(new_families) > args.top:
        add(f"\n…and {len(new_families) - args.top} more families "
            f"({sum(e['count'] for e in new_families[args.top:])} parents).")

    add(f"\n## Excluded — false gaps: {len(false_gaps)} families, "
        f"{sum(e['count'] for e in false_gaps)} parents\n")
    add("These pass the MRA test only because the cores that run them are "
        "console-style")
    add("and ship no MRAs. They run natively and **better** on MiSTer already; "
        "software")
    add("MAME is strictly worse for them (`feasibility.md` §5).\n")
    add("| family | parents | why excluded |")
    add("|---|---:|---|")
    for e in false_gaps:
        add(f"| `{e['family']}` | {e['count']} | {e['reason']} |")

    add(f"\n## Excluded — too heavy: {len(too_heavy)} families, "
        f"{sum(e['count'] for e in too_heavy)} parents\n")
    add("PS1-class and polygon 3D. `feasibility.md` §5: \"Do not count these as")
    add("unlocked.\" Listed so the exclusion is explicit and arguable rather than "
        "silent.\n")
    add("| family | parents | why excluded |")
    add("|---|---:|---|")
    for e in too_heavy:
        add(f"| `{e['family']}` | {e['count']} | {e['reason']} |")
    add(f"\n## Already reachable: {len(improved)} families, "
        f"{sum(e['count'] for e in improved)} parents\n")
    add("Uncovered by MiSTer but present in mame4all today — not a coverage win,")
    add("but where three years of driver fixes could matter.\n")
    add("| family | parents | example |")
    add("|---|---:|---|")
    for e in improved[:args.top]:
        add(f"| `{e['family']}` | {e['count']} | {e['example']} |")
    if len(improved) > args.top:
        add(f"\n…and {len(improved) - args.top} more families.")
    add(f"\n## Geometry risk: {len(geometry_risk)} sets wider than "
        f"{MAX_NATIVE_WIDTH} px\n")
    add("These cannot be presented at native geometry — the DDR reader's line")
    add("FIFO tops out at 512 pixels.\n")

    add("\n## Reproducing this\n")
    add("```")
    add("# listinfo XML, published with the reference romset:")
    add("curl -sL -o listinfo.xml \\")
    add("  'https://archive.org/download/mame-2003-plus-reference-set/"
        "mame2003-plus%20%5B2021-03-20%5D.xml'")
    add("")
    add("# mame4all setnames, offline from the vendored source (no device needed):")
    add("grep -hoE '^GAMEX?\\( *[0-9?]+, *[a-zA-Z0-9_]+' \\")
    add("  vendor/mame4all-pi/src/drivers/*.cpp | sed -E 's/.*, *//' | sort -u \\")
    add("  > mame4all-setnames.txt")
    add("")
    add("# MRA setnames: every <setname> and zip= under /media/fat/_Arcade.")
    add("# Two device traps: MRA filenames contain SPACES (so `for f in $(find …)`")
    add("# silently yields nothing) and the device has BusyBox grep (no --include).")
    add("")
    add("python3 tools/coverage-diff.py --listinfo listinfo.xml \\")
    add("  --mame4all mame4all-setnames.txt --mame4all-kind setnames \\")
    add("  --mra mra-setnames.txt")
    add("```\n")
    add("Verify the MRA list before trusting any of the above: `gng`, `sf2`, "
        "`pacman` and")
    add("`asteroid` must be present (MiSTer has cores) and `mk`, `klax` absent "
        "(Stage 8")
    add("benched them as gaps). Tests: `sh tests/coverage_diff_test.sh`.\n")

    report = "\n".join(out)
    print(report)

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"new_families": new_families,
                       "improved": improved,
                       "false_gaps": false_gaps,
                       "too_heavy": too_heavy,
                       "geometry_risk": geometry_risk}, fh, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
