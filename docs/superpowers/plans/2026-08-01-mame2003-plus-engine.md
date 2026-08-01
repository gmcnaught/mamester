# mame2003-plus Second Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Determine by measurement whether MAME 2003-Plus (0.78, libretro) should
replace or join mame4all-pi (0.37b5) as MAMESTer's emulation engine, by running it
on the same MiSTer DDR present path and launch harness.

**Architecture:** Extract the engine-agnostic MiSTer backend (DDR double-buffer,
timing publish, joystick words) out of mame4all's OSD glue into `nv_present.c/h`.
Build mame2003-plus as a static library for the Cortex-A9 and link it against a
purpose-built host that implements the libretro callbacks and calls the same
backend. Both engines then differ only in the emulator, which is what makes the
benchmark mean anything.

**Tech Stack:** C99 / C++98 (mame4all is C++), GNU make, Docker
(`crossbuild-essential-armhf` on x86_64), Python 3 (host-side analysis only),
POSIX sh (device-side triage), ALSA, `/dev/mem` MMIO.

**Spec:** [`../specs/2026-08-01-mame2003-plus-engine-design.md`](../specs/2026-08-01-mame2003-plus-engine-design.md)

## Global Constraints

- **Licence:** mame4all-pi and mame2003-plus are both the pre-2016 **non-commercial
  MAME licence**, not GPL. Source disclosure required; no commercial packaging; no
  ROM bundling; derivatives carry a distinct name. **Never commit a ROM or a
  romset zip.** ROMs live only on the device under `/media/fat/games/mame/roms/`.
- **Target silicon:** DE10-Nano HPS, dual Cortex-A9 @ 800 MHz, ARMv7-A, NEON
  (VFPv3, *not* NEON-VFPv4), hard float, armhf, glibc 2.31, MiSTer kernel 5.15.
  Compiler flags: `-marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard`.
- **Device:** `root@192.168.20.81`, passwordless SSH. `busybox devmem <addr> 32` for
  register peeks (`dd` is blocked by `CONFIG_STRICT_DEVMEM`). Screenshot via
  `echo screenshot > /dev/MiSTer_cmd`, newest PNG in
  `/media/fat/screenshots/MAMESTer/`. MAME **ignores SIGTERM** — use
  `timeout -s KILL` and `killall -9`.
- **Device is a shared resource.** Another session (`sweep`) also benchmarks on
  `.81`. Before any device step, confirm nothing of theirs is running
  (`ssh root@192.168.20.81 'ps w | grep -E "mame|docker"'`). An orphaned busy-loop
  pinning one of the two A9 cores silently corrupted an entire earlier sweep.
- **Benchmark protocol is non-negotiable** (`docs/bench-results.md`): per-cell spread
  on this device is 1.5–4%, and *cell order alone* moved one arm by 2%. Any
  comparison must be **interleaved, with alternating order, repeated**. Two blocks
  measured minutes apart will manufacture a difference of several percent. Never
  report a delta under ~5% from a non-interleaved run.
- **Bench configuration must match Stage 8** or the numbers are not comparable:
  FPGA core loaded, DDR present active, sound chips emulated, unthrottled, 600
  frames. A figure taken with no core loaded measures a different machine.
- **Do not touch branch `cortex-a9-build-flags`** — it belongs to the `sweep`
  session and backs PR #2.
- **Family-level judgement**, per the Stage 8 scope rule: a hardware family counts
  as covered if *any* of its drivers has a MiSTer MRA. Per-driver matching is
  useless because clone setnames are rarely named in MRAs. **NeoGeo is excluded by
  hand** — it has a wholesale core but ships `.neo` files, not MRAs.
- **Branching:** all work lands on `mame2003-plus-eval`, which must be rebased onto
  `presentfix` (Task 0) because the code being refactored lives there, not on `main`.

---

### Task 0: Base the work on the present-path fix

The extraction in Task 3 refactors `mister_video.cpp` **as fixed by the present-path
work**, which is on branch `presentfix` (`a79ea7c`), not on `main`. Building on
`main`'s copy would refactor the slow 8bpp path and silently discard a
hardware-verified optimisation.

**Files:**
- Modify: none (branch operation only)

**Interfaces:**
- Consumes: nothing
- Produces: a `mame2003-plus-eval` branch whose `tools/mame-frontend/mister-backend/mister_video.cpp` is the 23,046-byte fixed version and whose `tools/mister/Makefile.mister` lists `mister_profile.o` in `OSOBJS`

- [ ] **Step 1: Confirm what each branch holds**

```bash
git show presentfix:tools/mame-frontend/mister-backend/mister_video.cpp | wc -c
git show main:tools/mame-frontend/mister-backend/mister_video.cpp | wc -c
```

Expected: `23046` then `20100`. If `presentfix` is not 23046, stop — the branch is
not what this plan assumes.

- [ ] **Step 2: Rebase the eval branch onto it**

```bash
git checkout mame2003-plus-eval
git rebase presentfix
git log --oneline presentfix..HEAD
```

Expected: the spec commit `c724f6d` (or its rebased equivalent) is the only commit
listed.

- [ ] **Step 3: Verify the working tree now has the fixed backend**

```bash
wc -c tools/mame-frontend/mister-backend/mister_video.cpp
grep -c 'mister_profile.o' tools/mister/Makefile.mister
```

Expected: `23046` and `1`.

- [ ] **Step 4: Commit**

Nothing to commit — a rebase produces no new commit. Confirm cleanliness instead:

```bash
git status --short
```

Expected: only `m vendor/mame4all-pi` (a submodule pointer) and possibly untracked
build artefacts. No modified tracked files.

---

### Task 1: Coverage diff — what 2003-plus actually adds

Highest information per unit of effort, needs no build and almost no device time.
Its output picks the bench set for Task 8 and could reshape or cancel the project.

**Files:**
- Create: `tools/coverage-diff.py`
- Create: `tests/coverage_diff_test.sh`
- Create: `tests/fixtures/listinfo-sample.xml`
- Create: `tests/fixtures/mame4all-drivers-sample.txt`
- Create: `tests/fixtures/mra-setnames-sample.txt`
- Create: `docs/coverage-2003plus.md` (generated, committed)

**Interfaces:**
- Consumes: nothing
- Produces: `tools/coverage-diff.py --listinfo <xml> --mame4all <txt> --mra <txt> [--json <path>]`, writing a Markdown report to stdout. The JSON side-output has shape `{"new_families": [{"family": str, "parents": [str], "count": int}], "improved": [...], "geometry_risk": [...]}` — Task 8 reads `new_families` to pick representatives.

- [ ] **Step 1: Gather the three inputs**

The listinfo XML is published with the reference romset and needs no device:

```bash
mkdir -p /tmp/m2003p
curl -sL --max-time 300 -o /tmp/m2003p/listinfo.xml \
  "https://archive.org/download/mame-2003-plus-reference-set/mame2003-plus%20%5B2021-03-20%5D.xml"
wc -c /tmp/m2003p/listinfo.xml
```

Expected: about `20971520` bytes (20 MB).

The mame4all driver list comes off the device (read-only, no emulation, safe to run
while another session benchmarks):

```bash
ssh root@192.168.20.81 'cd /media/fat/games/mame && ./mame "*" -sourcefile' \
  > /tmp/m2003p/mame4all-drivers.txt
wc -l /tmp/m2003p/mame4all-drivers.txt
```

Expected: about `2270` lines.

**The MRA setname list already exists** — the `sweep` session generated it at
`/tmp/mame-ab/arcade-setnames.txt` (local, not on the device): 2,954 unique entries,
every `<setname>` and every `zip=` value under `/media/fat/_Arcade`, deduped and
sorted.

```bash
cp /tmp/mame-ab/arcade-setnames.txt /tmp/m2003p/mra-setnames.txt
wc -l /tmp/m2003p/mra-setnames.txt
for s in gng sf2 pacman asteroid mk klax; do
    printf "%-10s %s\n" "$s" "$(grep -cx "$s" /tmp/m2003p/mra-setnames.txt)"
done
```

Expected: `2954` lines, then `1` for gng/sf2/pacman/asteroid (MiSTer has cores) and
`0` for mk/klax (the gap games Stage 8 benched). If that pattern does not hold, the
list is wrong and every conclusion downstream of it is too.

To regenerate it, two device-specific traps, both learned the hard way: MRA
filenames under `_Arcade` contain **spaces**, so `for f in $(find ...)` word-splits
and silently yields nothing; and the device has **BusyBox grep**, which has no
`--include`. Use `find ... -exec grep -hoi ... {} +`.

**Path-prefixed entries can be ignored — verified, not assumed.** 31 of the 2,954
carry a prefix (28 `hbmame/`, 3 `sound/`). Basenaming them would change the setname
set by exactly two entries, `galnamco` and `pacupacu1`, both homebrew hacks that are
not 0.78 setnames; the other 26 hbmame names and all 3 `sound/` names
(`carnival`, `journey`, `pulsar`) already appear unprefixed. So stripping prefixes
is a no-op for coverage, and *not* stripping them avoids silently merging an hbmame
hack with its mainline namesake. `parse_mra` therefore skips any line containing
`/`, and reports how many it skipped.

- [ ] **Step 2: Write the fixtures**

`tests/fixtures/listinfo-sample.xml` — three games across two families, one a clone,
one already covered by an MRA:

```xml
<?xml version="1.0"?>
<mame>
	<game name="ridleofb" sourcefile="taito_f3.c">
		<description>Riddle of Pythagoras</description>
		<year>1994</year>
		<manufacturer>Taito</manufacturer>
		<video screen="raster" orientation="vertical" width="232" height="320" aspectx="3" aspecty="4" refresh="58.970000" />
		<driver status="good" color="good" sound="good" palettesize="8192" />
	</game>
	<game name="ridleofbc" sourcefile="taito_f3.c" cloneof="ridleofb" romof="ridleofb">
		<description>Riddle of Pythagoras (clone)</description>
		<year>1994</year>
		<manufacturer>Taito</manufacturer>
		<video screen="raster" orientation="vertical" width="232" height="320" aspectx="3" aspecty="4" refresh="58.970000" />
		<driver status="good" color="good" sound="good" palettesize="8192" />
	</game>
	<game name="gng" sourcefile="gng.c">
		<description>Ghosts'n Goblins</description>
		<year>1985</year>
		<manufacturer>Capcom</manufacturer>
		<video screen="raster" orientation="horizontal" width="256" height="224" aspectx="4" aspecty="3" refresh="59.590000" />
		<driver status="good" color="good" sound="good" palettesize="1024" />
	</game>
	<game name="wideone" sourcefile="wide.c">
		<description>Improbably Wide Game</description>
		<year>1992</year>
		<manufacturer>Nobody</manufacturer>
		<video screen="raster" orientation="horizontal" width="640" height="240" aspectx="4" aspecty="3" refresh="60.000000" />
		<driver status="good" color="good" sound="good" palettesize="256" />
	</game>
</mame>
```

`tests/fixtures/mame4all-drivers-sample.txt` — the `mame "*" -sourcefile` format is
`setname` then whitespace then the source file:

```
gng                      gng.c
wideone                  wide.c
```

`tests/fixtures/mra-setnames-sample.txt`:

```
gng
gngbl
```

With these fixtures the expected answer is: `taito_f3.c` is **new** (absent from
mame4all) and **uncovered** (no MRA), contributing 1 parent; `gng.c` is present in
mame4all and covered, so excluded; `wide.c` is in mame4all so not new, but its
640-pixel width is a geometry risk.

- [ ] **Step 2b: Write the failing test**

`tests/coverage_diff_test.sh`, following the style of the existing
`tests/game_manager_test.sh`:

```sh
#!/bin/sh
# coverage_diff_test.sh -- host-side tests for tools/coverage-diff.py
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
FIX="$HERE/fixtures"
pass=0; fail=0

check() { # check <label> <expected> <actual>
    if [ "$2" = "$3" ]; then
        pass=$((pass+1)); printf "ok   %s\n" "$1"
    else
        fail=$((fail+1)); printf "FAIL %s\n  expected: %s\n  actual:   %s\n" "$1" "$2" "$3"
    fi
}

OUT=$(python3 "$REPO/tools/coverage-diff.py" \
        --listinfo "$FIX/listinfo-sample.xml" \
        --mame4all "$FIX/mame4all-drivers-sample.txt" \
        --mra      "$FIX/mra-setnames-sample.txt" \
        --json     /tmp/coverage-diff-test.json) || { echo "FAIL script errored"; exit 1; }

check "taito_f3 reported as a new uncovered family" \
      "1" "$(python3 -c "import json;d=json.load(open('/tmp/coverage-diff-test.json'));print(sum(1 for f in d['new_families'] if f['family']=='taito_f3.c'))")"

check "clones do not inflate the parent count" \
      "1" "$(python3 -c "import json;d=json.load(open('/tmp/coverage-diff-test.json'));print([f['count'] for f in d['new_families'] if f['family']=='taito_f3.c'][0])")"

check "gng.c excluded (mame4all has it and an MRA covers it)" \
      "0" "$(python3 -c "import json;d=json.load(open('/tmp/coverage-diff-test.json'));print(sum(1 for f in d['new_families'] if f['family']=='gng.c'))")"

check "wide.c excluded from new families (mame4all has it)" \
      "0" "$(python3 -c "import json;d=json.load(open('/tmp/coverage-diff-test.json'));print(sum(1 for f in d['new_families'] if f['family']=='wide.c'))")"

check "640-wide game flagged as a geometry risk" \
      "1" "$(python3 -c "import json;d=json.load(open('/tmp/coverage-diff-test.json'));print(sum(1 for g in d['geometry_risk'] if g['name']=='wideone'))")"

check "report names the new family" \
      "1" "$(printf '%s' "$OUT" | grep -c 'taito_f3.c')"

printf "\n%d passed, %d failed\n" "$pass" "$fail"
[ "$fail" -eq 0 ]
```

- [ ] **Step 3: Run it and watch it fail**

```bash
sh tests/coverage_diff_test.sh
```

Expected: `FAIL script errored` — `tools/coverage-diff.py` does not exist.

- [ ] **Step 4: Write `tools/coverage-diff.py`**

```python
#!/usr/bin/env python3
"""Which hardware families would mame2003-plus add that nothing else covers?

Three inputs, all cheap to obtain and none requiring a build:

  --listinfo  the MAME-format XML shipped with the 2003-plus reference romset
  --mame4all  output of `mame "*" -sourcefile` from the shipped mame4all build
  --mra       every <setname> and zip= name under /media/fat/_Arcade, one per line

Judgement is at FAMILY level (the driver source file), per the Stage 8 scope rule:
a family counts as covered if ANY of its drivers has a MiSTer MRA. Per-driver
matching is useless because clone setnames are rarely named in MRAs, which made
cps1/pacman/system16 look like gaps.
"""

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET

# NeoGeo has a wholesale MiSTer core but ships .neo files rather than MRAs, so it
# never appears in the MRA setname list and would otherwise read as a huge gap.
HAND_EXCLUDED = {"neogeo.c"}

# The DDR reader's line FIFO tops out at 512 pixels; wider frames cannot be
# presented natively and fall back (docs/superpowers/progress.md, Stage 3).
MAX_NATIVE_WIDTH = 512


def parse_listinfo(path):
    """-> list of dicts, one per game entry."""
    games = []
    # iterparse keeps peak memory sane on the 20 MB document.
    for _event, elem in ET.iterparse(path, events=("end",)):
        if elem.tag != "game":
            continue
        video = elem.find("video")
        driver = elem.find("driver")
        desc = elem.find("description")
        games.append({
            "name": elem.get("name"),
            "family": elem.get("sourcefile"),
            "cloneof": elem.get("cloneof"),
            "description": desc.text if desc is not None else "",
            "width": int(video.get("width")) if video is not None and video.get("width") else None,
            "height": int(video.get("height")) if video is not None and video.get("height") else None,
            "refresh": float(video.get("refresh")) if video is not None and video.get("refresh") else None,
            "orientation": video.get("orientation") if video is not None else None,
            "status": driver.get("status") if driver is not None else None,
        })
        elem.clear()
    return games


def parse_mame4all(path):
    """`mame "*" -sourcefile` prints `setname<whitespace>source.c`.

    Returns (setnames, families).
    """
    setnames, families = set(), set()
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            parts = line.split()
            if len(parts) < 2:
                continue
            setnames.add(parts[0])
            families.add(parts[-1])
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
        print(f"<!-- parse_mra: skipped {skipped} path-prefixed entries -->",
              file=sys.stderr)
    return covered


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listinfo", required=True)
    ap.add_argument("--mame4all", required=True)
    ap.add_argument("--mra", required=True)
    ap.add_argument("--json")
    args = ap.parse_args()

    games = parse_listinfo(args.listinfo)
    m4a_sets, m4a_families = parse_mame4all(args.mame4all)
    mra_sets = parse_mra(args.mra)

    by_family = {}
    for g in games:
        by_family.setdefault(g["family"], []).append(g)

    new_families, improved, geometry_risk = [], [], []

    for family, members in sorted(by_family.items()):
        if family in HAND_EXCLUDED:
            continue
        covered_by_mister = any(g["name"] in mra_sets for g in members)
        in_mame4all = family in m4a_families
        parents = sorted(g["name"] for g in members if not g["cloneof"])

        if covered_by_mister:
            continue
        entry = {
            "family": family,
            "parents": parents,
            "count": len(parents),
            "example": next((g["description"] for g in members if not g["cloneof"]), ""),
        }
        if in_mame4all:
            # Already reachable today; 2003-plus may still emulate it better, but
            # it is not a coverage win.
            improved.append(entry)
        else:
            new_families.append(entry)

    for g in games:
        if g["width"] and g["width"] > MAX_NATIVE_WIDTH:
            geometry_risk.append({
                "name": g["name"], "family": g["family"],
                "width": g["width"], "height": g["height"],
            })

    new_families.sort(key=lambda e: -e["count"])
    improved.sort(key=lambda e: -e["count"])

    total_new_parents = sum(e["count"] for e in new_families)

    print("# mame2003-plus coverage diff\n")
    print(f"- 2003-plus game entries: **{len(games)}**")
    print(f"- distinct hardware families: **{len(by_family)}**")
    print(f"- mame4all families: **{len(m4a_families)}**, drivers: **{len(m4a_sets)}**")
    print(f"- MiSTer MRA setnames: **{len(mra_sets)}** (NeoGeo excluded by hand)\n")
    print(f"**Families 2003-plus adds that no MiSTer core covers: {len(new_families)}**")
    print(f"(**{total_new_parents}** parent romsets)\n")
    print("| family | parents | example |")
    print("|---|---:|---|")
    for e in new_families[:80]:
        print(f"| `{e['family']}` | {e['count']} | {e['example']} |")
    if len(new_families) > 80:
        print(f"\n…and {len(new_families) - 80} more families.\n")

    print(f"\n**Uncovered families mame4all already has: {len(improved)}** "
          f"({sum(e['count'] for e in improved)} parents) — not a coverage win, "
          "but where 3 years of driver fixes could matter.\n")

    print(f"\n**Wider than the reader's {MAX_NATIVE_WIDTH}-pixel line: "
          f"{len(geometry_risk)} sets** — these cannot present natively.\n")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"new_families": new_families,
                       "improved": improved,
                       "geometry_risk": geometry_risk}, fh, indent=1)

    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 5: Run the test and watch it pass**

```bash
sh tests/coverage_diff_test.sh
```

Expected: `6 passed, 0 failed`.

- [ ] **Step 6: Generate the real report**

```bash
python3 tools/coverage-diff.py \
  --listinfo /tmp/m2003p/listinfo.xml \
  --mame4all /tmp/m2003p/mame4all-drivers.txt \
  --mra      /tmp/m2003p/mra-setnames.txt \
  --json     /tmp/m2003p/coverage.json \
  > docs/coverage-2003plus.md
head -30 docs/coverage-2003plus.md
```

Sanity check the output before trusting it: `taito_f3.c` must appear in the new
families with roughly 30–40 parents, and `neogeo.c` must appear nowhere. If
`cps1.c` or `system16.c` show as new, the MRA list is wrong — re-derive it.

- [ ] **Step 7: Commit**

```bash
git add tools/coverage-diff.py tests/coverage_diff_test.sh tests/fixtures/ docs/coverage-2003plus.md
git commit -m "Coverage diff: what 2003-plus adds over mame4all and the MiSTer cores"
```

---

### Task 2: Cross-compile container, validated by rebuilding mame4all with it

2003-plus is 2,097 translation units against mame4all's 1,131, and 0.78 files are
larger. Under the existing qemu-emulated armhf container that is an afternoon per
iteration. A real cross toolchain fixes that — and building **both** engines with it
removes a confound that would otherwise sit inside the headline comparison, since
arm A and arm B must not differ by compiler.

**Files:**
- Create: `tools/mister/Dockerfile.cross-armhf`
- Modify: `tools/build-mame.sh`

**Interfaces:**
- Consumes: nothing
- Produces: `tools/build-mame.sh CROSS=1` builds `vendor/mame4all-pi/mame` using `arm-linux-gnueabihf-*`; Task 4 reuses the same image name `mamester-cross-armhf`

- [ ] **Step 1: Write the container**

`tools/mister/Dockerfile.cross-armhf`:

```dockerfile
# Real cross-compile container for the MiSTer ARM builds (x86_64 host).
#
# Replaces the qemu-emulated armhf container for anything large. mame2003-plus is
# 2,097 translation units; emulated, that is an afternoon per iteration.
#
# Debian multiarch gives us the armhf runtime -dev packages (SDL 1.2 and ALSA,
# which mame4all links) alongside an x86_64-hosted arm-linux-gnueabihf toolchain.
FROM debian:bullseye

RUN dpkg --add-architecture armhf \
    && apt-get update && apt-get install -y --no-install-recommends \
        crossbuild-essential-armhf \
        make \
        ca-certificates \
        libsdl1.2-dev:armhf \
        libasound2-dev:armhf \
        libglib2.0-dev:armhf \
    && rm -rf /var/lib/apt/lists/*

# mame4all's configure-less build finds SDL through sdl-config, which multiarch
# installs as the armhf one under this triplet-prefixed path.
ENV PATH="/usr/bin:${PATH}" \
    CROSS_PREFIX=arm-linux-gnueabihf-

WORKDIR /src
```

- [ ] **Step 2: Add a `CROSS=1` path to the build script**

Read `tools/build-mame.sh` first — it currently builds the qemu image and runs
`make -f Makefile.mister` inside it. Add, without disturbing the default path:

```sh
# CROSS=1 selects the real x86_64-hosted cross toolchain instead of qemu-emulated
# native. Same flags, same output; roughly an order of magnitude faster, which
# matters once mame2003-plus (2097 TUs) is in the picture. The default stays qemu
# so that existing verified builds remain byte-reproducible by the old path.
if [ "${CROSS:-0}" = "1" ]; then
    IMAGE=mamester-cross-armhf
    DOCKERFILE="$REPO/tools/mister/Dockerfile.cross-armhf"
    DOCKER_PLATFORM=""
    MAKE_ARGS="CC=arm-linux-gnueabihf-gcc CXX=arm-linux-gnueabihf-g++ LD=arm-linux-gnueabihf-g++ AR=arm-linux-gnueabihf-ar STRIPCMD=arm-linux-gnueabihf-strip"
else
    IMAGE=mamester-mame-build
    DOCKERFILE="$REPO/tools/mister/Dockerfile.mame-build"
    DOCKER_PLATFORM="--platform=linux/arm/v7"
    MAKE_ARGS=""
fi
```

Thread `$DOCKER_PLATFORM`, `$DOCKERFILE`, `$IMAGE` and `$MAKE_ARGS` through the
existing `docker build` and `docker run` invocations. Do not change the flags in
`Makefile.mister` — the point is that only the toolchain differs.

- [ ] **Step 3: Build and check the binary is the right kind of object**

```bash
CROSS=1 tools/build-mame.sh
file vendor/mame4all-pi/mame
readelf -A vendor/mame4all-pi/mame | grep -E 'Tag_CPU_name|Tag_FP_arch|Tag_ABI_VFP_args'
```

Expected: `ELF 32-bit LSB ... ARM, EABI5 ... dynamically linked, interpreter
/lib/ld-linux-armhf.so.3`; `Tag_CPU_name: "Cortex-A9"`; a VFP tag; and
`Tag_ABI_VFP_args: VFP registers` (hard float). If ABI_VFP_args is absent the build
is soft-float and **will not run** — fix before proceeding.

- [ ] **Step 4: Confirm it runs on the device**

```bash
ssh root@192.168.20.81 'ps w | grep -E "[m]ame|[d]ocker"'   # must be empty
scp vendor/mame4all-pi/mame root@192.168.20.81:/tmp/mame-cross
ssh root@192.168.20.81 'cd /media/fat/games/mame && SDL_VIDEODRIVER=dummy \
  MISTER_BENCH_FRAMES=120 timeout -s KILL 60 /tmp/mame-cross gng -rompath roms -nothrottle 2>&1 | tail -3'
```

Expected: a `MISTER-BENCH fps=` line. A segfault before the MAME banner usually
means a truncated transfer — check sha1 both sides.

- [ ] **Step 5: Quantify the toolchain delta, interleaved**

This is the step that licenses using the cross build for arm A later. Deploy the
shipped qemu-built binary as `/tmp/mame-qemu` alongside `/tmp/mame-cross`, load the
core, then alternate:

```bash
ssh root@192.168.20.81 'cd /media/fat/games/mame
for rep in 1 2 3; do
  for g in gng contra galaga klax; do
    for pair in "cross qemu" "qemu cross"; do
      [ $((rep % 2)) -eq 0 ] && set -- $pair || set -- $pair
      for which in $1 $2; do
        killall -9 mame 2>/dev/null; sleep 2
        printf "%s %s rep%s " "$g" "$which" "$rep"
        SDL_VIDEODRIVER=dummy MISTER_BENCH_FRAMES=600 timeout -s KILL 120 \
          /tmp/mame-$which "$g" -rompath roms -nothrottle 2>&1 \
          | grep -o "fps=[0-9.]*" | tail -1
      done
    done
  done
done'
```

Expected: per-game means within the 1.5–4% noise floor. Record the actual figures.
**If any game differs by more than ~5%, stop and report** — a toolchain that changes
throughput that much must be used for *both* engines or neither, and it also bears
on the `sweep` session's flag work.

- [ ] **Step 6: Commit**

```bash
git add tools/mister/Dockerfile.cross-armhf tools/build-mame.sh
git commit -m "Cross-compile container: real armhf toolchain instead of qemu"
```

---

### Task 3: Extract the engine-agnostic MiSTer backend

**Files:**
- Create: `tools/mame-frontend/mister-backend/nv_present.h`
- Create: `tools/mame-frontend/mister-backend/nv_present.c`
- Create: `tests/nv_present_test.c`
- Create: `tests/Makefile`
- Modify: `tools/mame-frontend/mister-backend/mister_video.cpp`
- Modify: `tools/mister/Makefile.mister`

**Interfaces:**
- Consumes: nothing
- Produces: the `nv_*` API below, linked by both `mister_video.cpp` (Task 3) and `libretro-host/` (Tasks 5–7)

- [ ] **Step 1: Write the header**

`tools/mame-frontend/mister-backend/nv_present.h`:

```c
/* nv_present.h -- the MiSTer present path, independent of any emulator.
 *
 * Both engines link this: mame4all-pi through mister_video.cpp's gp2x_* OSD glue,
 * and mame2003-plus through tools/mame-frontend/libretro-host/. Everything that
 * knows about the DDR contract at 0x3A000000 lives here and nowhere else.
 *
 * The contract (docs/superpowers/progress.md):
 *   ctrl word = (frame_counter << 2) | active_buf, published AFTER the pixels
 *   BUF0 at +0x40, BUF1 at +0x100040, timing block at +0x300000
 *   the counter must advance every frame or the stale-frame watchdog blanks
 */
#ifndef NV_PRESENT_H
#define NV_PRESENT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NV_FMT_RGB565   = 0,  /* already the DDR format; may go straight out    */
    NV_FMT_PAL8     = 1,  /* 8bpp indices, needs nv_set_palette()           */
    NV_FMT_XRGB8888 = 2,  /* libretro 32-bit, converted down to 565         */
    NV_FMT_0RGB1555 = 3   /* libretro 15-bit, converted up to 565           */
} nv_format;

/* Map /dev/mem and initialise. Returns 0 on success, negative errno on failure.
 * Honours MISTER_NO_NATIVE=1 by becoming a no-op present, which is how the
 * emulator is benchmarked with the present path removed. */
int  nv_open(void);
void nv_close(void);

/* Publish a modeline for this geometry and set the expected frame format.
 * `rot` is 0/90/180/270 clockwise, matching libretro's SET_ROTATION and
 * mame4all's -ror/-rol. Safe to call again when a driver changes mode. */
void nv_set_mode(int width, int height, double refresh_hz, int rot, nv_format fmt);

/* Palette for NV_FMT_PAL8, as RGB565 entries. Caller retains ownership. */
void nv_set_palette(const uint16_t *pal565, int entries);

/* Convert if needed, stage, and publish exactly one frame.
 * `pitch_bytes` is the source stride, which libretro cores do not guarantee to
 * equal width * bytes-per-pixel. */
void nv_frame(const void *src, int pitch_bytes);

/* Joystick word for player 0..3, as written back by the FPGA reader:
 * [0]right [1]left [2]down [3]up [4..9]fire1..6 [10]start [11]coin [12]pause */
uint32_t nv_pads(int player);

/* Frames published since nv_open(), for the bench counter. */
unsigned long nv_frame_count(void);

/* --- exposed for unit testing; not part of the present path's public use --- */
void nv_convert_1555_to_565(uint16_t *dst, const uint16_t *src, size_t px);
void nv_convert_8888_to_565(uint16_t *dst, const uint32_t *src, size_t px);
void nv_convert_pal8_to_565(uint16_t *dst, const uint8_t *src, size_t px,
                            const uint16_t *pal565);

#ifdef __cplusplus
}
#endif
#endif /* NV_PRESENT_H */
```

- [ ] **Step 2: Write the failing converter tests**

`tests/nv_present_test.c` — these run **natively on the host**, no device and no
`/dev/mem`, because the converters are pure functions:

```c
/* nv_present_test.c -- host-native unit tests for the format converters.
 * Build and run: make -C tests run */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../tools/mame-frontend/mister-backend/nv_present.h"

static int pass = 0, fail = 0;

static void check_u16(const char *label, uint16_t expected, uint16_t actual)
{
    if (expected == actual) { pass++; printf("ok   %s\n", label); }
    else { fail++; printf("FAIL %s: expected 0x%04x got 0x%04x\n", label, expected, actual); }
}

int main(void)
{
    /* 0RGB1555 -> RGB565: red and blue keep their 5 bits and shift; green gains a
     * bit, and the low green bit must be replicated from the top so that full
     * green stays full rather than landing one step short. */
    {
        uint16_t src[3] = { 0x7C00, 0x03E0, 0x001F };  /* red, green, blue */
        uint16_t dst[3] = { 0, 0, 0 };
        nv_convert_1555_to_565(dst, src, 3);
        check_u16("1555 full red  -> 565", 0xF800, dst[0]);
        check_u16("1555 full green-> 565", 0x07E0, dst[1]);
        check_u16("1555 full blue -> 565", 0x001F, dst[2]);
    }
    {
        uint16_t src[1] = { 0x0000 };
        uint16_t dst[1] = { 0xFFFF };
        nv_convert_1555_to_565(dst, src, 1);
        check_u16("1555 black -> 565", 0x0000, dst[0]);
    }

    /* XRGB8888 -> RGB565: truncate 8->5/6/5. */
    {
        uint32_t src[4] = { 0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFFFF };
        uint16_t dst[4] = { 0, 0, 0, 0 };
        nv_convert_8888_to_565(dst, src, 4);
        check_u16("8888 full red  -> 565", 0xF800, dst[0]);
        check_u16("8888 full green-> 565", 0x07E0, dst[1]);
        check_u16("8888 full blue -> 565", 0x001F, dst[2]);
        check_u16("8888 white     -> 565", 0xFFFF, dst[3]);
    }
    {
        /* Mid grey 0x808080 truncates to 5/6/5 as 16/32/16. */
        uint32_t src[1] = { 0x00808080 };
        uint16_t dst[1] = { 0 };
        nv_convert_8888_to_565(dst, src, 1);
        check_u16("8888 mid grey -> 565", (uint16_t)((16 << 11) | (32 << 5) | 16), dst[0]);
    }

    /* PAL8 -> RGB565 is a table lookup; the point of the test is that it is a
     * straight indexed gather with no clamping surprise at index 255. */
    {
        uint16_t pal[256];
        uint8_t  src[4] = { 0, 1, 128, 255 };
        uint16_t dst[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < 256; i++) pal[i] = (uint16_t)(i * 257);
        nv_convert_pal8_to_565(dst, src, 4, pal);
        check_u16("pal8 index 0",   pal[0],   dst[0]);
        check_u16("pal8 index 1",   pal[1],   dst[1]);
        check_u16("pal8 index 128", pal[128], dst[2]);
        check_u16("pal8 index 255", pal[255], dst[3]);
    }

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
```

`tests/Makefile`:

```make
# Host-native tests. These deliberately do NOT cross-compile: the converters are
# pure functions and testing them on the build machine is faster and needs no
# device. Anything touching /dev/mem is verified on hardware instead.
CFLAGS ?= -std=c99 -O1 -Wall -Wextra -g
BACKEND := ../tools/mame-frontend/mister-backend

nv_present_test: nv_present_test.c $(BACKEND)/nv_present.c $(BACKEND)/nv_present.h
	$(CC) $(CFLAGS) -DNV_HOST_TEST=1 -o $@ nv_present_test.c $(BACKEND)/nv_present.c

.PHONY: run clean
run: nv_present_test
	./nv_present_test

clean:
	rm -f nv_present_test
```

- [ ] **Step 3: Run the tests and watch them fail**

```bash
make -C tests run
```

Expected: a compile error — `nv_present.c` does not exist yet.

- [ ] **Step 4: Write `nv_present.c`**

Move the implementation across from `mister_video.cpp` rather than rewriting it:
`nv_init`, `nv_configure`, `nv_present`, `nv_convert_row`, the staging-frame
allocation, the `nv_poll_pads`/`gp2x_joystick_read` DDR reads, and the
`nv_modeline.h` calls. Preserve two behaviours exactly, both of them measured
rather than guessed (`docs/bench-results.md`):

1. **8bpp/converted output goes via a cached staging frame, then one `memcpy` into
   DDR.** Per-pixel stores into the uncached `/dev/mem` window run at 24.8 MB/s
   versus 89 MB/s for a `memcpy` — that was 15.1 ms per 512×384 frame and 65% of
   process CPU.
2. **`NV_FMT_RGB565` writes DDR directly, with no staging copy.** Staging a frame
   that is already in the destination format cost `mk` 2.5% for nothing.

The converters themselves, which are what the tests pin:

```c
void nv_convert_1555_to_565(uint16_t *dst, const uint16_t *src, size_t px)
{
    for (size_t i = 0; i < px; i++) {
        const uint16_t s = src[i];
        const uint16_t r = (uint16_t)((s >> 10) & 0x1F);
        const uint16_t g = (uint16_t)((s >>  5) & 0x1F);
        const uint16_t b = (uint16_t)( s        & 0x1F);
        /* green gains a bit: replicate the top bit into the new low bit so that
         * 0x1F maps to 0x3F rather than 0x3E. */
        dst[i] = (uint16_t)((r << 11) | ((g << 1) | (g >> 4)) << 5 | b);
    }
}

void nv_convert_8888_to_565(uint16_t *dst, const uint32_t *src, size_t px)
{
    for (size_t i = 0; i < px; i++) {
        const uint32_t s = src[i];
        dst[i] = (uint16_t)(((s >> 8) & 0xF800) |
                            ((s >> 5) & 0x07E0) |
                            ((s >> 3) & 0x001F));
    }
}

void nv_convert_pal8_to_565(uint16_t *dst, const uint8_t *src, size_t px,
                            const uint16_t *pal565)
{
    for (size_t i = 0; i < px; i++)
        dst[i] = pal565[src[i]];
}
```

Guard the `/dev/mem` mapping so the host tests link: wrap `nv_open`'s body in
`#ifndef NV_HOST_TEST`, returning 0 in the test build.

- [ ] **Step 5: Run the tests and watch them pass**

```bash
make -C tests run
```

Expected: `13 passed, 0 failed`.

- [ ] **Step 6: Reduce `mister_video.cpp` to glue and rebuild**

`mister_video.cpp` keeps only mame4all's `gp2x_*` entry points, now delegating:
`gp2x_set_video_mode` → `nv_set_mode`, `gp2x_video_flip`/`DisplayScreen` →
`nv_frame`, `gp2x_joystick_read` → `nv_pads`, palette updates → `nv_set_palette`.
Add `$(OBJ)/mister/nv_present.o` to `OSOBJS` in `tools/mister/Makefile.mister`
beside `mister_video.o` and `mister_profile.o`, and extend the `cp` lines in
`tools/build-mame.sh` if they do not already glob `*.c`.

```bash
CROSS=1 tools/build-mame.sh
file vendor/mame4all-pi/mame
```

Expected: links clean, ARM EABI5 hard-float.

- [ ] **Step 7: Prove on hardware that the refactor changed nothing**

Correctness first — one 8bpp driver and one 16bpp driver, because the staging
carve-out is the thing most likely to have been broken:

```bash
ssh root@192.168.20.81 'echo "load_core /media/fat/_Other/MAMESTer_20260731.rbf" > /dev/MiSTer_cmd'
# run 720 (8bpp, 512x384) and mk (16bpp, 416x254), screenshot each, scp back and LOOK at them
ssh root@192.168.20.81 'echo screenshot > /dev/MiSTer_cmd'
```

Expected: both render correctly through the scaler, as they did before the
refactor. A frozen frame is not the same as a correct one — take two screenshots
seconds apart and compare MD5s to confirm animation.

Then speed, interleaved, alternating order, three repetitions, against the
pre-refactor binary on the same eight games as Task 2 Step 5.

Expected: within the 1.5–4% noise floor. This is a mechanical refactor; a real
delta means something moved that should not have.

- [ ] **Step 8: Commit**

```bash
git add tools/mame-frontend/mister-backend/nv_present.c \
        tools/mame-frontend/mister-backend/nv_present.h \
        tools/mame-frontend/mister-backend/mister_video.cpp \
        tools/mister/Makefile.mister tools/build-mame.sh tests/
git commit -m "Extract the MiSTer present path into engine-agnostic nv_present.c"
```

---

### Task 4: Build mame2003-plus as a static library for the Cortex-A9

**Files:**
- Create: `vendor/mame2003-plus/` (git submodule)
- Create: `tools/mister/Makefile.m2003p`
- Create: `tools/build-m2003p.sh`

**Interfaces:**
- Consumes: the `mamester-cross-armhf` image from Task 2
- Produces: `vendor/mame2003-plus/mame2003_plus_libretro.a`, exporting the standard `retro_*` symbols; Task 5 links against it

- [ ] **Step 1: Vendor the source**

```bash
git submodule add https://github.com/libretro/mame2003-plus-libretro.git vendor/mame2003-plus
git -C vendor/mame2003-plus rev-parse --short HEAD
```

Record that SHA in the results doc later — driver behaviour is version-dependent
and "mame2003-plus" alone is not a reproducible statement.

- [ ] **Step 2: Add the MiSTer platform block**

`tools/mister/Makefile.m2003p` is included by the upstream Makefile via
`platform=mister`. Model it on the existing `s812` block (upstream `Makefile`
lines 292–312), which is already a Cortex-A9 NEON target, reconciled with the flags
settled for this device:

```make
# MiSTer DE10-Nano HPS: dual Cortex-A9 @800MHz, ARMv7-A, NEON (VFPv3, NOT vfpv4),
# hard float, glibc 2.31. Closest upstream target is s812, which is also an A9.
#
# STATIC_LINKING=1 makes the build emit an .a (Makefile:907, `ar rcs`) instead of
# a shared object, so tools/mame-frontend/libretro-host/ can link the core
# directly -- no dlopen, no RetroArch.
TARGET  := mame2003_plus_libretro.a
STATIC_LINKING := 1

PLATCFLAGS += -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
              -fomit-frame-pointer -fno-math-errno

HAVE_NEON  = 1
ARCH       = arm
CPU_ARCH  := arm
ARM        = 1
HAVE_ARMv6 = 1

# The hand-written ARM CPU cores. These are the reason this comparison exists:
# our mame4all build runs with BOTH disabled (Cyclone segfaults on entry for every
# 68000 driver, DrZ80 crashes in DrZ80Run for sound CPUs -- see
# tools/mister/patches/0002-default-asm-cores-off.patch), while 2003-plus ships the
# same two cores and enables them on ARM. Task 8 benches with and without, so this
# must be switchable from the command line rather than hardcoded.
USE_CYCLONE ?= 1
USE_DRZ80   ?= 1
```

- [ ] **Step 3: Write the build script**

`tools/build-m2003p.sh`, mirroring `tools/build-mame.sh`:

```sh
#!/bin/sh
# Build mame2003-plus as a static library for the MiSTer HPS (Cortex-A9).
#
#   tools/build-m2003p.sh                 # ASM CPU cores on (default)
#   USE_CYCLONE=0 USE_DRZ80=0 tools/build-m2003p.sh   # portable C cores
#
# Output: vendor/mame2003-plus/mame2003_plus_libretro.a
#
# 2097 translation units -- this uses the real cross toolchain from
# tools/mister/Dockerfile.cross-armhf, not the qemu-emulated container.
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
SRC="$REPO/vendor/mame2003-plus"
IMAGE=mamester-cross-armhf
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

[ -f "$SRC/Makefile" ] || { echo "vendor/mame2003-plus is empty -- git submodule update --init"; exit 1; }

cp "$REPO/tools/mister/Makefile.m2003p" "$SRC/Makefile.mister-platform"

docker build --platform=linux/amd64 -t "$IMAGE" -f "$REPO/tools/mister/Dockerfile.cross-armhf" "$REPO/tools/mister"

docker run --rm --platform=linux/amd64 -v "$SRC:/src" -w /src "$IMAGE" \
    make -j"$JOBS" platform=unix \
        CC=arm-linux-gnueabihf-gcc \
        CXX=arm-linux-gnueabihf-g++ \
        AR=arm-linux-gnueabihf-ar \
        STATIC_LINKING=1 \
        USE_CYCLONE="${USE_CYCLONE:-1}" \
        USE_DRZ80="${USE_DRZ80:-1}" \
        TARGET_NAME=mame2003_plus \
        TARGET=mame2003_plus_libretro.a \
        fpic= \
        "PLATCFLAGS=-marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fomit-frame-pointer -fno-math-errno" \
        HAVE_NEON=1 ARM=1 ARCH=arm

ls -lh "$SRC"/*.a
```

Three of those assignments are load-bearing, and the reason they work is that
**neither `Makefile` nor `Makefile.common` contains a single `override` directive** —
so a command-line assignment beats every makefile assignment to the same variable,
including `+=`:

- `PLATCFLAGS=...` replaces the platform block's flags wholesale rather than
  appending. That is what is wanted here: the full flag set is supplied. Verified
  that the `unix` block never assigns `PLATCFLAGS` at all, so nothing is lost.
- `TARGET=...a` is required because the `unix` block sets
  `TARGET = $(TARGET_NAME)_libretro.so`. Without this, `STATIC_LINKING=1` still runs
  `ar rcs` (Makefile:907) but names the result `.so` — an ar archive wearing a shared
  object's extension, which links fine and confuses everyone who looks at it later.
- `fpic=` clears the `-fPIC` that the `unix` block appends to `CFLAGS`. Position-
  independent code costs a register and an indirection on ARM for no benefit in a
  statically linked binary. `-shared` also lands in `LDFLAGS`, but that is harmless
  because `STATIC_LINKING=1` archives instead of linking.

Passing flags this way avoids patching upstream, which keeps the submodule clean.

- [ ] **Step 4: Build it**

```bash
time tools/build-m2003p.sh
```

Expected: `mame2003_plus_libretro.a`, on the order of 100–200 MB unstripped. First
build is long even cross-compiled; subsequent ones are incremental.

- [ ] **Step 5: Verify the archive is the right architecture and exports the API**

```bash
ar t vendor/mame2003-plus/mame2003_plus_libretro.a | wc -l
cd /tmp && ar x /path/to/mame2003_plus_libretro.a mame2003.o && \
  readelf -A mame2003.o | grep -E 'Tag_CPU_name|Tag_ABI_VFP_args'
arm-linux-gnueabihf-nm -g --defined-only vendor/mame2003-plus/mame2003_plus_libretro.a \
  | grep -cE ' T retro_(init|run|load_game|get_system_av_info|set_video_refresh)$'
```

Expected: about 2,097 objects; `Tag_CPU_name: "Cortex-A9"` and
`Tag_ABI_VFP_args: VFP registers`; and `5` for the symbol count. A soft-float
object here will not link against the hard-float host.

- [ ] **Step 6: Commit**

```bash
git add .gitmodules vendor/mame2003-plus tools/mister/Makefile.m2003p tools/build-m2003p.sh
git commit -m "Build mame2003-plus as a Cortex-A9 static library"
```

---

### Task 5: libretro host — lifecycle and an early answer on speed

The deliberate ordering win: this task produces an fps number for 2003-plus
**before** the present path, input or audio exist. If 0.78 is hopeless on this
silicon, that shows up here and Tasks 6–9 are never written.

**Files:**
- Create: `tools/mame-frontend/libretro-host/host_main.c`
- Create: `tools/mame-frontend/libretro-host/host_env.c`
- Create: `tools/mame-frontend/libretro-host/host.h`
- Create: `tools/mame-frontend/libretro-host/Makefile`

**Interfaces:**
- Consumes: `vendor/mame2003-plus/mame2003_plus_libretro.a` (Task 4)
- Produces: binary `mame2003`, CLI `mame2003 <setname> [-rompath DIR] [-frames N] [-nopresent]`; and `host.h` declaring `host_env_init(const char *system_dir, const char *save_dir)`, `host_video_refresh(const void*, unsigned, unsigned, size_t)`, `host_set_pixel_format(unsigned)`, `host_set_rotation(unsigned)`, `host_geometry_changed(const struct retro_system_av_info*)` — Task 6 implements the video ones for real

- [ ] **Step 1: Write the environment callback**

`host_env.c` must answer exactly the 21 calls the core makes (verified by grep over
`src/mame2003/*.c`). The ones that matter:

| call | required response |
|---|---|
| `SET_PIXEL_FORMAT` | store the format, return true — the core picks RGB565, 0RGB1555 or XRGB8888 per driver |
| `GET_SYSTEM_DIRECTORY` / `GET_SAVE_DIRECTORY` | return `games/mame/`; the core falls back to the content directory if these are NULL, which would scatter nvram into `roms/` |
| `GET_VARIABLE` | serve a **pinned** option table (below) |
| `GET_CORE_OPTIONS_VERSION` | write `0` — this makes the core use the legacy `SET_VARIABLES` path and avoids implementing the v1/v2 option structures |
| `SET_ROTATION` | store; feeds `nv_set_mode`'s `rot` in Task 6 |
| `SET_SYSTEM_AV_INFO` / `SET_GEOMETRY` | re-publish the modeline in Task 6 |
| `GET_LOG_INTERFACE` | supply a `vfprintf`-to-stderr logger; without it the core's diagnostics are invisible and every failure looks identical |
| `GET_PERF_INTERFACE`, `GET_LED_INTERFACE`, `GET_VFS_INTERFACE`, `SET_AUDIO_BUFFER_STATUS_CALLBACK` | return false; all optional |
| `SET_MESSAGE`, `SET_INPUT_DESCRIPTORS`, `SET_CONTROLLER_INFO`, `SET_PERFORMANCE_LEVEL`, `SET_VARIABLES`, `SET_CORE_OPTIONS*` | accept and ignore, return true |
| `GET_VARIABLE_UPDATE` | write `false` |

Pin every option explicitly. An unpinned option means the core's own default
silently decides what is being measured, and `frameskip` or `samplerate` moving
between arms would invalidate the whole benchmark:

```c
/* Pinned so that every benchmark arm measures the same emulator. Recorded
 * verbatim in docs/bench-results-2003plus.md. */
static const struct { const char *key; const char *value; } host_options[] = {
    { "mame2003-plus_frameskip",           "0"        },
    { "mame2003-plus_samplerate",          "44100"    },  /* matches mame4all's rate */
    { "mame2003-plus_cpu_clock_scale",     "default"  },
    { "mame2003-plus_skip_disclaimer",     "enabled"  },
    { "mame2003-plus_skip_warnings",       "enabled"  },
    { "mame2003-plus_dcs_speedhack",       "enabled"  },
    { "mame2003-plus_sample_rate",         "44100"    },
    { NULL, NULL }
};
```

Verify these key names against `src/mame2003/mame2003_core_options.h` in the
submodule before building — a mistyped key silently falls through to the core
default, which is exactly the failure this table exists to prevent.

- [ ] **Step 2: Write the main loop**

`host_main.c`: parse argv, build the romset path as `<rompath>/<setname>.zip`
(the core derives the setname from the basename minus extension, and requires the
path to exist), then:

```c
    retro_set_environment(host_environment);
    retro_set_video_refresh(host_video_refresh);
    retro_set_audio_sample_batch(host_audio_batch);
    retro_set_audio_sample(host_audio_sample);
    retro_set_input_poll(host_input_poll);
    retro_set_input_state(host_input_state);

    retro_init();

    struct retro_game_info game = { rom_path, NULL, 0, NULL };
    if (!retro_load_game(&game)) {
        fprintf(stderr, "MISTER-HOST: retro_load_game failed for %s\n", rom_path);
        return 2;
    }

    struct retro_system_av_info av;
    retro_get_system_av_info(&av);
    fprintf(stderr, "MISTER-HOST: %ux%u @ %.4f Hz, %.0f Hz audio\n",
            av.geometry.base_width, av.geometry.base_height,
            av.timing.fps, av.timing.sample_rate);

    for (unsigned long f = 0; !frame_limit || f < frame_limit; f++)
        retro_run();
```

`retro_get_system_info` reports `need_fullpath`; assert it is true, because the
host passes a path and never loads the zip into memory.

Report fps in the **same format the existing tooling greps for** —
`MISTER-BENCH fps=%.1f` — so `gap-triage.sh`'s parser works unchanged for both
engines.

- [ ] **Step 3: Stub video and audio for now**

`host_video_refresh` counts frames and returns. `host_audio_batch` returns
`frames` without writing anywhere. **Do not disable sound in the core options** —
the sound chips must still be emulated, because that CPU cost is part of what is
being measured. Only the ALSA write is absent, worth 1–3%.

- [ ] **Step 4: Build and link**

```bash
make -C tools/mame-frontend/libretro-host CROSS=1
file tools/mame-frontend/libretro-host/mame2003
```

Expected: ARM EABI5 hard-float dynamically linked ELF. An undefined-symbol error
naming `retro_*` means Task 4's archive is stale; one naming `nv_*` means
`nv_present.o` is not in the link line.

- [ ] **Step 5: First run on the device — the early answer**

```bash
ssh root@192.168.20.81 'ps w | grep -E "[m]ame|[d]ocker"'   # must be empty
scp tools/mame-frontend/libretro-host/mame2003 root@192.168.20.81:/media/fat/games/mame/
ssh root@192.168.20.81 'cd /media/fat/games/mame && \
  ./mame2003 gng -rompath roms -frames 600 2>&1 | tail -5'
```

The 0.37b5 romset in `roms/` will **not** load under 0.78 — expect
`retro_load_game failed`. Fetch a 2003-plus set first (Task 8 Step 1 builds the
script; for this one run, do it by hand):

```bash
ssh root@192.168.20.81 'cd /media/fat/games/mame && mkdir -p roms2003 && \
  wget -q -O roms2003/gng.zip \
    "https://archive.org/download/mame-2003-plus-reference-set/roms/gng.zip" && \
  ./mame2003 gng -rompath roms2003 -frames 600 2>&1 | tail -5'
```

Expected: a geometry line, then `MISTER-BENCH fps=`. **Record this number.** It is
the first direct 0.78-vs-0.37b5 signal, though not yet comparable to the Stage 8
bands — no core is loaded and no present is happening, so compare it only against
mame4all run the same way (`MISTER_NO_NATIVE=1`).

- [ ] **Step 6: Commit**

```bash
git add tools/mame-frontend/libretro-host/
git commit -m "libretro host: lifecycle, environment callback and a frame counter"
```

---

### Task 6: Video — libretro frames into the DDR present path

**Files:**
- Create: `tools/mame-frontend/libretro-host/host_video.c`
- Modify: `tools/mame-frontend/libretro-host/host_main.c`
- Modify: `tools/mame-frontend/libretro-host/Makefile`

**Interfaces:**
- Consumes: `nv_set_mode`, `nv_frame` (Task 3); `host_set_pixel_format`, `host_set_rotation`, `host_geometry_changed` (Task 5)
- Produces: a running game on the MiSTer's output at native geometry

- [ ] **Step 1: Map libretro pixel formats onto `nv_format`**

```c
/* The core picks its format per driver in mame2003_video_init_conversion()
 * (src/mame2003/video.c): depth 16 without VIDEO_NEEDS_6BITS_PER_GUN becomes
 * RGB565 -- the common case, and already the DDR format. Six-bits-per-gun and
 * depth 32 become XRGB8888; depth 15 becomes 0RGB1555. Both of those need a
 * convert that mame4all never paid, so watch them when a driver benches slow. */
static nv_format host_fmt_from_retro(unsigned retro_fmt)
{
    switch (retro_fmt) {
    case RETRO_PIXEL_FORMAT_RGB565:   return NV_FMT_RGB565;
    case RETRO_PIXEL_FORMAT_0RGB1555: return NV_FMT_0RGB1555;
    case RETRO_PIXEL_FORMAT_XRGB8888: return NV_FMT_XRGB8888;
    default:                          return NV_FMT_RGB565;
    }
}
```

- [ ] **Step 2: Publish geometry and present**

```c
void host_video_refresh(const void *data, unsigned width, unsigned height,
                        size_t pitch)
{
    /* libretro permits a NULL frame to mean "duplicate the last one". The DDR
     * stale-frame watchdog blanks the screen if the counter stops advancing, so
     * a dupe must still be published rather than skipped. */
    if (!data) { nv_frame(NULL, 0); return; }

    if (width != cur_width || height != cur_height) {
        cur_width = width; cur_height = height;
        nv_set_mode((int)width, (int)height, cur_refresh_hz, cur_rotation, cur_fmt);
    }
    nv_frame(data, (int)pitch);
}
```

`pitch` is a **byte** stride and libretro cores do not guarantee it equals
`width * bpp`. Passing it through is why `nv_frame` takes `pitch_bytes` rather than
deriving it.

- [ ] **Step 3: Verify on hardware — correctness before speed**

Pick three drivers deliberately, one per format path, using the format each driver
selects (visible in the core's log line at startup): an RGB565 driver, a 0RGB1555
driver, and an XRGB8888 (six-bits-per-gun) driver.

```bash
ssh root@192.168.20.81 'echo "load_core /media/fat/_Other/MAMESTer_20260731.rbf" > /dev/MiSTer_cmd'
ssh root@192.168.20.81 'cd /media/fat/games/mame && ./mame2003 gng -rompath roms2003 -frames 1800 &'
sleep 5 && ssh root@192.168.20.81 'echo screenshot > /dev/MiSTer_cmd'
sleep 3 && ssh root@192.168.20.81 'echo screenshot > /dev/MiSTer_cmd'
```

scp both PNGs back and **look at them**. Expected: correct colours, correct
geometry, no centre-clip, and **different MD5s** between the two — identical MD5s
mean a frozen frame, which the earlier stages learned to distinguish the hard way.
Wrong colours on exactly one of the three drivers points at that format's converter,
which Task 3's unit tests cover — add the failing case there.

- [ ] **Step 4: Commit**

```bash
git add tools/mame-frontend/libretro-host/
git commit -m "libretro host: present frames through the MiSTer DDR path"
```

---

### Task 7: Input, audio and throttle — make it playable

**Files:**
- Create: `tools/mame-frontend/libretro-host/host_input.c`
- Create: `tools/mame-frontend/libretro-host/host_audio.c`
- Create: `tools/mame-frontend/libretro-host/host_throttle.c`
- Modify: `tools/mame-frontend/libretro-host/host_main.c`

**Interfaces:**
- Consumes: `nv_pads` (Task 3)
- Produces: `mame2003` playable with a pad and audible, launchable by the existing harness

- [ ] **Step 1: Map the joystick words to libretro buttons**

The FPGA reader writes back one word per player, and Stage 5 fixed the bit layout
against the MiSTer arcade-core convention. Map straight across — no remapping layer,
because the CONF_STR already names these buttons:

```c
/* nv_pads() bit -> RETRO_DEVICE_ID_JOYPAD_*, per the Stage 5 convention:
 * [0]right [1]left [2]down [3]up [4..9]fire1..6 [10]start [11]coin [12]pause */
static const int pad_bit_to_retro[] = {
    [0]  = RETRO_DEVICE_ID_JOYPAD_RIGHT,
    [1]  = RETRO_DEVICE_ID_JOYPAD_LEFT,
    [2]  = RETRO_DEVICE_ID_JOYPAD_DOWN,
    [3]  = RETRO_DEVICE_ID_JOYPAD_UP,
    [4]  = RETRO_DEVICE_ID_JOYPAD_B,
    [5]  = RETRO_DEVICE_ID_JOYPAD_A,
    [6]  = RETRO_DEVICE_ID_JOYPAD_Y,
    [7]  = RETRO_DEVICE_ID_JOYPAD_X,
    [8]  = RETRO_DEVICE_ID_JOYPAD_L,
    [9]  = RETRO_DEVICE_ID_JOYPAD_R,
    [10] = RETRO_DEVICE_ID_JOYPAD_START,
    [11] = RETRO_DEVICE_ID_JOYPAD_SELECT,
};
```

2003-plus supports up to 4 players (`MAX_PLAYER_COUNT`), and `nv_pads` already
serves four words, so all four ports wire up identically — the same reason Stage 5
put Start and Coin on each player's own pad.

- [ ] **Step 2: Audio to ALSA**

Reuse what Stage 6 established rather than rediscovering it, because the failure
mode is obscure: `snd_pcm_set_params`' latency-derived buffer is **rejected** by
MiSTer's ALSA chain for many (rate, refresh) pairs including 44100 at 60 Hz
("Unable to get period size"). Request the same configuration explicitly through
`hw_params` instead: **one period per emulated frame** (`sample_rate / fps`, e.g.
735 at 44100/60) and four of them. The only card is `Dummy`, a `model_MiSTer`
build that is 48 kHz stereo only; the `plug` layer converts.

libretro hands over interleaved stereo `int16_t` via
`retro_audio_sample_batch(const int16_t *data, size_t frames)`, which matches
`snd_pcm_writei` directly. Write non-blocking and count underruns rather than
stalling the emulation loop; report them at exit.

- [ ] **Step 3: Throttle to the driver's refresh**

Unthrottled is a benchmark mode, not a play mode. Pace on
`av.timing.fps` (e.g. 59.59 for gng, 53.2 for mk) with `clock_nanosleep` on
`CLOCK_MONOTONIC`. Two device-specific traps: this is armhf, so a 32-bit `long`
holding nanoseconds overflows — use `int64_t`/`struct timespec` throughout; and
`-frames`/`MISTER_BENCH_FRAMES` must bypass the throttle entirely.

- [ ] **Step 4: Verify on hardware**

Input, the same way Stage 5 proved it: poke a sentinel into each joystick word,
confirm the FPGA overwrites it, then press every button and check the core sees it.
Audio: confirm ALSA reaches `RUNNING` and `/dev/MrAudio` is held by `mame2003`, then
**ask the operator to confirm it is audible** — no automated check substitutes.
Throttle: a driver whose native refresh is not 60 Hz (mk at ~53.2) should report
close to its own rate, not 60.

- [ ] **Step 5: Commit**

```bash
git add tools/mame-frontend/libretro-host/
git commit -m "libretro host: pad input, ALSA audio and native-refresh throttle"
```

---

### Task 8: The four-arm benchmark

**Files:**
- Create: `tools/gap-triage-2003.sh`
- Create: `docs/bench-results-2003plus.md`

**Interfaces:**
- Consumes: `new_families` from Task 1's JSON; the binaries from Tasks 3–7
- Produces: the per-driver tax table the decision rests on

- [ ] **Step 1: Write the romset fetcher**

`tools/gap-triage-2003.sh` is `gap-triage.sh` with a different base URL and binary.
Keep the verdict classification identical so results are comparable:

```sh
#!/bin/sh
# gap-triage-2003.sh -- fetch a 2003-plus romset and classify how the driver runs.
# Runs ON THE DEVICE. ROMs stay there; never commit them.
#
#   sh gap-triage-2003.sh ridleofb gseeker ...
BASE="https://archive.org/download/mame-2003-plus-reference-set/roms"
GAMEDIR="${GAMEDIR:-/media/fat/games/mame}"
ROMDIR="${ROMDIR:-roms2003}"
FRAMES="${FRAMES:-600}"
TIMEOUT="${TIMEOUT:-90}"
BIN="${BIN:-./mame2003}"

cd "$GAMEDIR" || exit 1
mkdir -p "$ROMDIR"

for g in "$@"; do
    if [ ! -f "$ROMDIR/$g.zip" ]; then
        # curl's TLS fails on this device; wget works.
        wget -q -O "$ROMDIR/$g.zip.part" "$BASE/$g.zip" \
            && mv "$ROMDIR/$g.zip.part" "$ROMDIR/$g.zip" \
            || { rm -f "$ROMDIR/$g.zip.part"; printf "%-12s DOWNLOAD FAILED\n" "$g"; continue; }
    fi

    killall -9 mame mame2003 2>/dev/null; sleep 1
    MISTER_BENCH_FRAMES="$FRAMES" timeout -s KILL "$TIMEOUT" \
        "$BIN" "$g" -rompath "$ROMDIR" -frames "$FRAMES" >/tmp/g2003.log 2>&1
    rc=$?
    fps=$(grep MISTER-BENCH /tmp/g2003.log | tail -1 | sed 's/.*fps=\([0-9.]*\).*/\1/')

    if   [ -n "$fps" ];                            then verdict="OK fps=$fps"
    elif grep -q "retro_load_game failed" /tmp/g2003.log; then verdict="ROMSET/DRIVER"
    elif [ "$rc" = 137 ];                          then verdict="HANG (killed at ${TIMEOUT}s)"
    else verdict="EXIT rc=$rc: $(grep -v '^$' /tmp/g2003.log | tail -1 | cut -c1-70)"
    fi
    printf "%-12s %s\n" "$g" "$verdict"
done
killall -9 mame mame2003 2>/dev/null
```

- [ ] **Step 2: Build the four arms**

| arm | binary | build |
|---|---|---|
| A | `mame-c` | mame4all, portable C cores — the shipped default |
| B | `mame2003-c` | `USE_CYCLONE=0 USE_DRZ80=0 tools/build-m2003p.sh` |
| C | `mame2003-asm` | `tools/build-m2003p.sh` (defaults on) |
| D | `mame-asm` | mame4all with `-cyclone -drz80_snd` at runtime; one run only |

All four cross-compiled with the same toolchain (Task 2), so the only difference is
the emulator.

- [ ] **Step 3: Run bench set 1 — overlap**

`gng contra galaga klax mk nbajam gunbird 720`. Covers Z80, 68000, TMS34010 and the
marginal band. Configuration: **core loaded, present active, sound emulated,
unthrottled, 600 frames** — anything else is not comparable to Stage 8.

Interleave arms within each game, alternate the order between repetitions, three
repetitions minimum. Report mean and spread per cell. A cell whose spread exceeds
5% is not a result; re-run it.

- [ ] **Step 4: Run arm D once, for the diagnosis**

mame4all with `-cyclone` on any 68000 driver in the set (klax, mk). Expected: a
segfault on entry to emulation, per `0002-default-asm-cores-off.patch`. If arm C
ran those same drivers clean with the same Cyclone core, the fault is in mame4all's
*integration* rather than the core — record that, because it points at recoverable
speed for the engine already shipping. Not a goal; a by-product worth one run.

- [ ] **Step 5: Run bench set 2 — the new families**

One parent per family from Task 1's `new_families`, prioritised by parent count.
Expect `taito_f3` among the largest. Same configuration. Classify against the Stage 8
band: ≥75 fps healthy, 60–75 marginal, below 60 will not hold.

Note explicitly in the results how many families were sampled and how many were not.
Silent truncation reads as "covered everything" when it did not.

- [ ] **Step 6: Write the results document**

`docs/bench-results-2003plus.md`. Record, alongside the numbers: the submodule SHA
from Task 4, the pinned core-option table from Task 5, the toolchain delta from
Task 2, and the exact bench configuration. State plainly which conclusions the data
supports and which it does not.

- [ ] **Step 7: Commit**

```bash
git add tools/gap-triage-2003.sh docs/bench-results-2003plus.md
git commit -m "Four-arm benchmark: mame2003-plus against mame4all on the same present path"
```

---

### Task 9: Decide, and write the decision down

**Files:**
- Modify: `docs/superpowers/progress.md`
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: everything
- Produces: a recorded decision among replace / both engines / stop

- [ ] **Step 1: Put the decision to the operator with the evidence**

Present the tax table, the new-family results and the coverage numbers. The three
outcomes, from the spec:

- **Replace** — 2003-plus is at least as fast across the overlap set and the new
  families run. mame4all retires.
- **Both engines** — 2003-plus costs more on titles mame4all already runs well but
  unlocks the new families; `game_manager.sh` picks per setname. The host binary,
  launch path, present backend and opts mechanism are already shared, so this is
  cheap. Judged the likeliest outcome when the spec was written.
- **Stop** — the tax is large and the new families do not clear real time. The
  coverage diff and the extracted backend survive regardless.

Do not pick on the operator's behalf. The criterion is theirs.

- [ ] **Step 2: Record it in the ledger**

Add a Stage 9 section to `docs/superpowers/progress.md` covering what was built,
what the four arms measured, what the coverage diff found, and the decision with its
reasoning. Follow the existing sections' style: state what was *verified* and how,
separately from what was inferred.

- [ ] **Step 3: Update `CLAUDE.md` if the engine choice changed**

The "Settled architecture" section names mame4all-pi as the v1 video build target
and 2003-Plus as a future upgrade. If the decision moves that, update it — and say
plainly what the measurement was, so the next session does not relitigate it.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/progress.md CLAUDE.md
git commit -m "Ledger: mame2003-plus evaluation result and engine decision"
```

---

## Self-review notes

**Spec coverage.** Component 1 → Task 3. Component 2 → Tasks 5–7. Component 3 →
Tasks 2 and 4. Component 4 → Task 1. Component 5 → Task 8 Step 1. The four-arm bench
→ Task 8. The side finding on Cyclone/DrZ80 → Task 8 Step 4. The decision gate →
Task 9. Task 0 covers a branch dependency the spec did not state explicitly.

**One deviation from the spec, deliberate.** The spec left the mame4all arm on its
existing qemu-built binary. Task 2 rebuilds it with the cross toolchain and measures
the difference first, because otherwise arm A and arm B differ by compiler as well as
by emulator, and the headline comparison would carry a confound that no amount of
interleaving removes.

**Verified against upstream while writing this plan**, so the steps rest on facts
rather than assumption:

- `STATIC_LINKING=1` emits a real archive via `ar rcs` (`Makefile:907`).
- Neither `Makefile` nor `Makefile.common` uses `override`, so the command-line
  assignments in Task 4 Step 3 take effect; the `unix` block sets `TARGET` and
  `fpic` but never `PLATCFLAGS`.
- `retro_load_game` takes the **full path to the romset zip** and derives the setname
  from the basename minus extension; rompath is the zip's dirname. The host never
  reads the zip itself.
- The core makes exactly 21 distinct environment calls; Task 5 Step 1 enumerates all
  of them.
- The core converts palettised output to RGB565 internally (`VCT_PALTO565`,
  `src/mame2003/video.c`), so the common path needs no conversion in the host at all.

**Unverified and worth watching.** The core-option key names in Task 5 Step 1 are
written from the upstream naming convention, not read out of
`mame2003_plus_core_options.h`. That file must be checked before the first build —
a mistyped key silently falls back to the core default, which is precisely the
failure the pinned table exists to prevent.
