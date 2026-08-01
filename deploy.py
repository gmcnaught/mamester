#!/usr/bin/env python3
"""
Deploy the MAMESTer MiSTer port (emulator + launch harness + FPGA core) to a
running MiSTer over SSH.

Modeled on the sibling ports' deploy.py — plain ssh/scp (the device is
SSH-key-authed, so `ssh root@<HOST>` needs no password) with sha1-verified
transfers. The verification is not ceremony: /media/fat is exFAT and a partial
scp leaves a TRUNCATED file, and a truncated ELF segfaults before main() with no
output at all.

The three pieces:

  1. RBF       the FPGA core, _Other/MAMESTer_YYYYMMDD.rbf — gitignored, built by
               .github/workflows/build-rbf.yml. Fetch the newest with
               `gh run download <id> -n mame-rbf -D _Other`. The
               lexicographically-last name wins (the dates sort chronologically).
  2. EMULATORS both engines, since the per-driver choice is made at launch:
               tools/mame-frontend/libretro-host/mame2003 — MAME 2003-Plus, the
               PRIMARY engine (make -C tools/mame-frontend/libretro-host CROSS=1),
               and vendor/mame4all-pi/mame — mame4all-pi, the fallback for
               drivers 2003-Plus cannot run acceptably (tools/build-mame.sh).
               Both carry the same present/input/audio backend (nv_present.c).
  3. HARNESS   games/MAMESTer/ — _handler.sh (Master_Daemon entry point),
               game_manager.sh + game_lib.sh (OSD game selection).

Device tree — two directories, and they are not interchangeable:

  /media/fat/games/MAMESTer/     HOMEDIR: name must equal the CONF_STR setname,
    _handler.sh                  because that is how Master_Daemon finds the
    game_manager.sh              handler and where the OSD file browser opens.
    game_lib.sh                  Launch harness only.
    roms -> ../mame/roms         so the browser can reach the romsets

  /media/fat/games/mame/         GAMEDIR: the emulator and its data. MAME
    mame                         chdir()s to its own binary's directory at
    mame.cfg                     startup, so this is the cwd its relative
    roms/                        config paths resolve against.
    opts/<setname>.opt           per-game launch flags (user-owned)
    cfg/ nvram/ hi/ inp/ snap/ samples/    MAME state

  /media/fat/_Other/MAMESTer_*.rbf                               <- RBF
  /media/fat/logs/MAMESTer/                                      logs

ROMs are NEVER shipped by this script — user-supplied, and the pre-2016 MAME
license forbids bundling them.

LAUNCH: MiSTer's Master_Daemon (Frontier) watches /tmp/CORENAME and runs
games/MAMESTer/_handler.sh when the core loads. Never the MiSTer.ini `main=`
wrapper — that replaces MiSTer's main() and inherits its FPGA-readiness contract
to the core, which the sibling ports measured wedging frame 1.

Usage:
  ./deploy.py                    RBF + emulator + harness
  ./deploy.py --no-rbf           emulator + harness only
  ./deploy.py --harness-only     just the shell scripts (fast iteration)
  ./deploy.py --rbf FILE         ship an explicit RBF
  ./deploy.py --load             load the core when the deploy finishes
  ./deploy.py --host 1.2.3.4     override the device address
"""

import argparse
import glob
import hashlib
import os
import shlex
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent
DEFAULT_HOST = os.environ.get("MISTER_HOST", "192.168.20.81")

HOMEDIR = "/media/fat/games/MAMESTer"   # setname dir: handler + OSD browser home
GAMEDIR = "/media/fat/games/mame"       # emulator + ROMs + MAME state
OTHERDIR = "/media/fat/_Other"
LOGDIR = "/media/fat/logs/MAMESTer"

# Directories the core needs. The handler creates these too — doing it here as
# well means a fresh device is set up before the first core load.
DEVICE_DIRS = [
    HOMEDIR, GAMEDIR, LOGDIR,
    f"{GAMEDIR}/roms", f"{GAMEDIR}/opts", f"{GAMEDIR}/samples",
    f"{GAMEDIR}/cfg", f"{GAMEDIR}/nvram", f"{GAMEDIR}/hi",
    f"{GAMEDIR}/inp", f"{GAMEDIR}/snap",
]

# (local path, remote path, executable, keep-device-copy-if-present)
HARNESS = [
    ("games/MAMESTer/_handler.sh",     f"{HOMEDIR}/_handler.sh",     True,  False),
    ("games/MAMESTer/game_manager.sh", f"{HOMEDIR}/game_manager.sh", True,  False),
    ("games/MAMESTer/game_lib.sh",     f"{HOMEDIR}/game_lib.sh",     False, False),
    ("games/mame/mame.cfg",            f"{GAMEDIR}/mame.cfg",        False, True),
    ("games/mame/opts/README.md",      f"{GAMEDIR}/opts/README.md",  False, False),
]


# ---------------------------------------------------------------- ssh plumbing

class Device:
    def __init__(self, host, dry_run=False):
        self.host = host
        self.dry_run = dry_run

    def run(self, command, check=True, quiet=False):
        """Run a shell command on the device; returns stdout, stripped."""
        if self.dry_run and not quiet:
            print(f"  [dry-run] ssh {self.host} {command!r}")
            return ""
        proc = subprocess.run(
            ["ssh", "-o", "ConnectTimeout=10", f"root@{self.host}", command],
            capture_output=True, text=True,
        )
        if check and proc.returncode != 0:
            raise RuntimeError(
                f"ssh failed ({proc.returncode}): {command}\n{proc.stderr.strip()}"
            )
        return proc.stdout.strip()

    def copy(self, local: Path, remote: str):
        if self.dry_run:
            print(f"  [dry-run] scp {local} -> {remote}")
            return
        subprocess.run(["scp", "-q", str(local), f"root@{self.host}:{remote}"],
                       check=True)


def sha1(path: Path) -> str:
    h = hashlib.sha1()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def push(dev: Device, local: Path, remote: str, executable=False, keep_existing=False):
    """scp one file and verify its sha1 on the device."""
    if not local.is_file():
        raise FileNotFoundError(local)

    if keep_existing and dev.run(f"test -f {shlex.quote(remote)} && echo yes",
                                 check=False, quiet=True) == "yes":
        print(f"  keep   {remote}  (present; --force-cfg to overwrite)")
        return

    want = sha1(local)
    size = local.stat().st_size
    have = dev.run(f"sha1sum {shlex.quote(remote)} 2>/dev/null | cut -d' ' -f1",
                   check=False, quiet=True)
    if have == want:
        print(f"  same   {remote}  ({size:,} B)")
    else:
        dev.copy(local, remote)
        if not dev.dry_run:
            got = dev.run(f"sha1sum {shlex.quote(remote)} | cut -d' ' -f1")
            if got != want:
                raise RuntimeError(
                    f"sha1 mismatch after copying {remote}: want {want}, got {got} "
                    "— truncated transfer, do NOT run this build"
                )
        verb = "would" if dev.dry_run else "sent "
        print(f"  {verb}  {remote}  ({size:,} B, sha1 {want[:12]})")

    if executable:
        dev.run(f"chmod +x {shlex.quote(remote)}")


# ------------------------------------------------------------------- artifacts

def newest_rbf():
    found = sorted(glob.glob(str(REPO / "_Other" / "MAMESTer_*.rbf")))
    return Path(found[-1]) if found else None


def check_daemon(dev: Device):
    """Master_Daemon is what launches the handler; without it nothing starts."""
    running = dev.run("ps w | grep -c '[M]aster_Daemon.sh'", check=False, quiet=True)
    if running and running not in ("0", ""):
        print("  ok     Master_Daemon is running")
        return
    print("  WARNING: Master_Daemon.sh is not running on the device.")
    print("           Nothing auto-launches when the core loads. Install it with")
    print("           /media/fat/Scripts/Install_MiSTer_Frontier.sh, or start it:")
    print("           bash /media/fat/MiSTer_Frontier/Master_Daemon.sh &")


def link_roms(dev: Device):
    """Make the romsets reachable from the OSD browser's home directory.

    The browser opens at the core's games/<setname>/ directory (HOMEDIR), but
    the romsets live with the emulator in GAMEDIR. The device's exFAT mount
    carries symlinks, and Main_MiSTer's browser stat()s DT_LNK entries and shows
    them as directories (file_io.cpp ScanDirectory), so one symlink is enough —
    no copying a 940-zip romset around.
    """
    link = f"{HOMEDIR}/roms"
    if dev.run(f"test -L {shlex.quote(link)} && echo yes",
               check=False, quiet=True) == "yes":
        print(f"  roms   {link} -> {dev.run(f'readlink {shlex.quote(link)}', quiet=True)}")
        return
    if dev.run(f"test -e {shlex.quote(link)} && echo yes", check=False, quiet=True) == "yes":
        print(f"  roms   {link} exists and is not a symlink — leaving it alone")
        return
    dev.run(f"ln -s {shlex.quote(GAMEDIR)}/roms {shlex.quote(link)}")
    print(f"  roms   {link} -> {GAMEDIR}/roms")


def check_roms(dev: Device):
    count = dev.run(f"ls {shlex.quote(GAMEDIR)}/roms/*.zip 2>/dev/null | wc -l",
                    check=False, quiet=True)
    if count and count != "0":
        print(f"  ok     {count} romsets in {GAMEDIR}/roms")
        return
    loose = dev.run(f"ls {shlex.quote(GAMEDIR)}/*.zip 2>/dev/null | wc -l",
                    check=False, quiet=True)
    if loose and loose != "0":
        print(f"  WARNING: {loose} romsets sit directly in {GAMEDIR}, not in roms/.")
        print(f"           The OSD browser only reaches {HOMEDIR}/roms -> {GAMEDIR}/roms,")
        print(f"           so move them:  ssh root@HOST 'mv {GAMEDIR}/*.zip {GAMEDIR}/roms/'")
        return
    print(f"  WARNING: no .zip romsets under {GAMEDIR}/roms — the OSD 'Load Game'")
    print("           browser will be empty. Copy 0.37b5 romsets in.")


# ------------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser(
        description="Deploy the MAMESTer MiSTer port.",
        epilog=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--rbf", type=Path, help="explicit RBF instead of the newest in _Other/")
    ap.add_argument("--no-rbf", action="store_true", help="skip the FPGA core")
    ap.add_argument("--no-bin", action="store_true", help="skip the emulator binary")
    ap.add_argument("--harness-only", action="store_true",
                    help="only the shell scripts (fast iteration on the launch path)")
    ap.add_argument("--force-cfg", action="store_true",
                    help="overwrite mame.cfg on the device (default: keep local edits)")
    ap.add_argument("--load", action="store_true", help="load the core when finished")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    dev = Device(args.host, args.dry_run)
    print(f"MAMESTer deploy -> root@{args.host}")

    if not args.dry_run:
        print("  device " + dev.run("uname -sm").replace("\n", " ")
              + "  core=" + (dev.run("cat /tmp/CORENAME 2>/dev/null", check=False) or "?"))

    dev.run("mkdir -p " + " ".join(shlex.quote(d) for d in DEVICE_DIRS))

    do_bin = not (args.no_bin or args.harness_only)
    do_rbf = not (args.no_rbf or args.harness_only)

    rbf = None
    if do_rbf:
        rbf = args.rbf or newest_rbf()
        if rbf is None:
            print("  ERROR: no _Other/MAMESTer_*.rbf found. Build one with the\n"
                  "         'Build MAMESTer RBF' workflow and fetch it with\n"
                  "           gh run download <id> -n mame-rbf -D _Other\n"
                  "         or pass --no-rbf to deploy the ARM side only.")
            return 1

    print("\nharness")
    for rel, remote, ex, keep in HARNESS:
        push(dev, REPO / rel, remote, executable=ex,
             keep_existing=keep and not args.force_cfg)

    if do_bin:
        print("\nemulators")
        # Two engines, and MAME 2003-Plus is the primary one: a driver ships on
        # it wherever it reaches an acceptable frame rate, with mame4all-pi
        # underneath for the rest. Both are pushed, because the per-driver
        # choice is made at launch time (games/MAMESTer/game_manager.sh), not
        # at deploy time.
        engines = [
            (REPO / "tools" / "mame-frontend" / "libretro-host" / "mame2003",
             f"{GAMEDIR}/mame2003", "MAME 2003-Plus (primary)",
             "make -C tools/mame-frontend/libretro-host CROSS=1"),
            (REPO / "vendor" / "mame4all-pi" / "mame",
             f"{GAMEDIR}/mame", "mame4all-pi (fallback)",
             "tools/build-mame.sh"),
        ]
        missing = [(p, why, how) for p, _, why, how in engines if not p.is_file()]
        if len(missing) == len(engines):
            for p, why, how in missing:
                print(f"  ERROR: {p.relative_to(REPO)} is missing ({why}) — "
                      f"build it with {how}")
            return 1
        for path, remote, why, how in engines:
            if path.is_file():
                push(dev, path, remote, executable=True)
            else:
                # One engine present is a usable device; a driver that needed
                # the absent one simply will not launch. Loud, not fatal.
                print(f"  WARNING: {path.relative_to(REPO)} missing ({why}) — "
                      f"not deployed; build with {how}")

    if do_rbf:
        print("\ncore")
        push(dev, rbf, f"{OTHERDIR}/{rbf.name}")

    if not args.dry_run:
        print("\nchecks")
        link_roms(dev)
        check_daemon(dev)
        check_roms(dev)

    if args.load:
        rbf_name = f"{OTHERDIR}/{rbf.name}" if rbf else \
            dev.run(f"ls -1 {OTHERDIR}/MAMESTer_*.rbf | tail -1", quiet=True)
        print(f"\nloading {rbf_name}")
        dev.run(f'echo "load_core {rbf_name}" > /dev/MiSTer_cmd')
        print("  pick a romset from the OSD ('Load Game') to start it")

    print("\ndone")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, FileNotFoundError, subprocess.CalledProcessError) as exc:
        print(f"\nFAILED: {exc}", file=sys.stderr)
        sys.exit(1)
