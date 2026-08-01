#!/usr/bin/env python3
"""
Generate MiSTer .mgl shortcuts so MAMESTer romsets appear in MiSTer's own menu.

Without these, a game is chosen from inside the core ("Load Game" in the OSD).
An .mgl is the MiSTer-standard alternative: selecting one loads the core AND
mounts the romset in one action, so games show up in the main menu next to every
other system's titles.

    <mistergamedescription>
        <rbf>_Other/MAMESTer_20260801</rbf>
        <file delay="2" type="s" index="0" path="roms/gng.zip"/>
    </mistergamedescription>

The path is relative to the core's home directory (/media/fat/games/MAMESTer) —
device-observed: Main_MiSTer joins it onto that home dir before storing the
result in MAMESTer.s0, which games/MAMESTer/game_manager.sh then resolves. The
2-second delay is measured from core load and must stay long enough for the
handler's FPGA-settle sleep; the manager anchors its "is this pick new?" test on
/tmp/CORENAME's mtime so it cannot miss an early mount.

Titles come from `mame -listfull` on the device, so the menu reads "Ghosts'n
Goblins (World? set 1)" rather than "gng".

SPACE: this device's exFAT allocates 128 KB per file, so ~940 shortcuts cost
~120 MB of card for a few hundred bytes of content. Generate a curated subset
unless you actually want the whole romset in the menu.

Usage:
  tools/make-mgl.py --match "gng,1943,mk"      just these setnames
  tools/make-mgl.py --match "kof*"             glob against setname or title
  tools/make-mgl.py --all                      every romset present (see SPACE)
  tools/make-mgl.py --list                     show what would be written
  tools/make-mgl.py --clean                    remove the generated directory
"""

import argparse
import fnmatch
import re
import shlex
import subprocess
import sys

DEFAULT_HOST = "192.168.20.81"
HOMEDIR = "/media/fat/games/MAMESTer"
GAMEDIR = "/media/fat/games/mame"
OTHERDIR = "/media/fat/_Other"
# A top-level _-prefixed directory becomes an entry in MiSTer's main menu.
MGLDIR = "/media/fat/_MAMESTer"

MGL = """<mistergamedescription>
\t<rbf>_Other/{rbf}</rbf>
\t<file delay="2" type="s" index="0" path="roms/{zip}"/>
</mistergamedescription>
"""

# MiSTer's browser shows the filename; keep it to characters exFAT accepts.
UNSAFE = re.compile(r'[<>:"/\\|?*\x00-\x1f]')


def ssh(host, command, check=True):
    proc = subprocess.run(
        ["ssh", "-o", "ConnectTimeout=10", f"root@{host}", command],
        capture_output=True, text=True,
    )
    if check and proc.returncode != 0:
        sys.exit(f"ssh failed: {command}\n{proc.stderr.strip()}")
    return proc.stdout


def romsets(host):
    """Setnames present on the device, as {setname: zip filename}."""
    out = ssh(host, f"ls -1 {shlex.quote(GAMEDIR)}/roms/*.zip 2>/dev/null")
    found = {}
    for line in out.splitlines():
        name = line.rsplit("/", 1)[-1]
        if name.lower().endswith(".zip"):
            found[name[:-4]] = name
    return found


def titles(host):
    """setname -> description, from mame's own driver table."""
    out = ssh(host, f"cd {shlex.quote(GAMEDIR)} && ./mame -listfull 2>/dev/null",
              check=False)
    table = {}
    for line in out.splitlines():
        # "name      "Description"
        m = re.match(r'^(\S+)\s+"(.*)"\s*$', line)
        if m:
            table[m.group(1)] = m.group(2)
    return table


def safe_name(text):
    return UNSAFE.sub("-", text).strip().rstrip(".")


def main():
    ap = argparse.ArgumentParser(
        description="Generate MiSTer .mgl shortcuts for MAMESTer romsets.",
        epilog=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--match", help="comma-separated setnames or globs "
                                    "(matched against setname and title)")
    ap.add_argument("--all", action="store_true", help="every romset present")
    ap.add_argument("--list", action="store_true", help="show, do not write")
    ap.add_argument("--clean", action="store_true", help=f"remove {MGLDIR} and exit")
    ap.add_argument("--rbf", help="RBF basename without .rbf "
                                  "(default: newest MAMESTer_* on the device)")
    args = ap.parse_args()

    if args.clean:
        ssh(args.host, f"rm -rf {shlex.quote(MGLDIR)}")
        print(f"removed {MGLDIR}")
        return 0

    if not (args.all or args.match):
        ap.error("choose --match <names> or --all (see --help for the space cost)")

    rbf = args.rbf or ssh(
        args.host, f"ls -1 {OTHERDIR}/MAMESTer_*.rbf | tail -1").strip()
    if not rbf:
        sys.exit(f"no MAMESTer_*.rbf found in {OTHERDIR} — deploy a core first")
    rbf = rbf.rsplit("/", 1)[-1]
    if rbf.endswith(".rbf"):
        rbf = rbf[:-4]

    present = romsets(args.host)
    if not present:
        sys.exit(f"no romsets in {GAMEDIR}/roms")
    table = titles(args.host)

    if args.all:
        chosen = sorted(present)
    else:
        patterns = [p.strip() for p in args.match.split(",") if p.strip()]
        # Exact setname or an explicit glob — no implicit trailing "*", so
        # "1943" means 1943 and "1943*" also brings in 1943kai/1943mii. Titles
        # are matched too, so --match "*ghosts*" works.
        def hit(setname, pattern):
            return (fnmatch.fnmatch(setname, pattern)
                    or fnmatch.fnmatch(table.get(setname, "").lower(), pattern.lower()))

        chosen = sorted(s for s in present if any(hit(s, p) for p in patterns))
        missing = [p for p in patterns if not any(hit(s, p) for s in chosen)]
        for p in missing:
            print(f"  no romset matched '{p}'", file=sys.stderr)

    if not chosen:
        sys.exit("nothing matched")

    print(f"rbf     {rbf}")
    print(f"target  {MGLDIR}  ({len(chosen)} shortcuts)")
    if args.list:
        for s in chosen:
            print(f"  {s:<12} {table.get(s, '(no title)')}")
        return 0

    ssh(args.host, f"mkdir -p {shlex.quote(MGLDIR)}")
    # One ssh for the lot: a per-file round trip over 900 games is minutes.
    script = []
    for s in chosen:
        title = safe_name(table.get(s) or s)
        body = MGL.format(rbf=rbf, zip=present[s])
        script.append(f"cat > {shlex.quote(f'{MGLDIR}/{title}.mgl')} <<'MGLEOF'\n"
                      f"{body}MGLEOF")
    ssh(args.host, "\n".join(script))

    written = ssh(args.host, f"ls -1 {shlex.quote(MGLDIR)}/*.mgl | wc -l").strip()
    print(f"wrote   {written} .mgl files")
    print(f"        they appear in MiSTer's main menu under _MAMESTer")
    return 0


if __name__ == "__main__":
    sys.exit(main())
