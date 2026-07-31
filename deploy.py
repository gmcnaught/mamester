#!/usr/bin/env python3
"""
Deploy the MAMESTer MiSTer port to a running MiSTer over SSH.

STATUS: SKELETON. The port has no build artifacts yet, so this script cannot
deploy an emulator or a core — it wires up the transport (ssh/scp + sha1 verify),
documents the intended device tree, and installs the launch handler. Every
artifact-copying step is a clearly-marked TODO that refuses rather than pretends.

Modeled on solarus-mister/deploy.py and maldita.castilla-mister/deploy.py: plain
ssh/scp (the device is SSH-key-authed, so `ssh root@<HOST>` needs no password),
sha1-verified transfers (a partial scp to FAT can leave a TRUNCATED file; a
truncated ELF segfaults before main with no output, so every binary is verified).

Intended device tree (once artifacts exist):

  /media/fat/_Other/MAMESTer_YYYYMMDD.rbf    <- RBF (FPGA passthrough core)
  /media/fat/games/mame/
      mame                                    <- emulator ELF (armhf, static)
      roms/                                   <- ROMs (USER-SUPPLIED; never shipped)
  /media/fat/games/MAMESTer/_handler.sh           <- launch handler (this repo)

AUTO-LAUNCH: the emulator is started by MiSTer's Master_Daemon, which watches
/tmp/CORENAME and runs games/<setname>/_handler.sh when the core loads. Stock
MiSTer main keeps running (and keeps its FPGA-readiness contract). Do NOT use a
MiSTer.ini `main=` wrapper — the sibling ports measured it causing frame-1 wedges.

Usage:
  ./deploy.py --dry-run        show what would happen (default until artifacts exist)
  ./deploy.py --handler-only   install just games/MAMESTer/_handler.sh (works today)
  ./deploy.py                  full deploy (NOT YET — errors: no artifacts)
"""

import argparse
import hashlib
import os
import subprocess
import sys

HOST = os.environ.get("MISTER_HOST", "root@192.168.20.81")  # MiSTer device (see CLAUDE.md)
REPO = os.path.dirname(os.path.abspath(__file__))

# Core setname == RBF CONF_STR first token == /tmp/CORENAME. The handler dir
# name MUST equal it (Master_Daemon runs games/<CORENAME>/_handler.sh). This is
# the core display name; the emulator ELF + ROMs live under lowercase games/mame.
SETNAME = "MAMESTer"
HANDLER_SRC = os.path.join(REPO, "games", SETNAME, "_handler.sh")
HANDLER_DST = f"/media/fat/games/{SETNAME}/_handler.sh"


def sh(cmd, **kw):
    """Run a command, echoing it. Raises on nonzero exit."""
    print("  $", " ".join(cmd))
    return subprocess.run(cmd, check=True, **kw)


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def scp_verified(local, remote, dry_run):
    """scp a file, then verify its on-device sha1 matches (FAT truncation guard)."""
    want = sha1(local)
    print(f"  {os.path.basename(local)}  sha1={want[:12]}…  -> {HOST}:{remote}")
    if dry_run:
        return
    sh(["ssh", HOST, f"mkdir -p {os.path.dirname(remote)!r}"])
    sh(["scp", "-q", local, f"{HOST}:{remote}"])
    got = subprocess.run(
        ["ssh", HOST, f"sha1sum {remote!r}"],
        check=True, capture_output=True, text=True,
    ).stdout.split()[0]
    if got != want:
        sys.exit(f"ERROR: sha1 mismatch after scp ({got[:12]}… != {want[:12]}…) — "
                 f"transfer truncated; retry.")
    print("    verified OK")


def install_handler(dry_run):
    if not os.path.exists(HANDLER_SRC):
        sys.exit(f"ERROR: {HANDLER_SRC} missing")
    scp_verified(HANDLER_SRC, HANDLER_DST, dry_run)
    if not dry_run:
        sh(["ssh", HOST, f"chmod +x {HANDLER_DST!r}"])


def deploy_artifacts(dry_run):
    # TODO: once builds exist, deploy in this order:
    #   1. RBF     newest _Other/MAMESTer_*.rbf  -> /media/fat/_Other/   (scp_verified)
    #   2. ELF     tools build output        -> /media/fat/games/mame/mame
    #   3. handler games/MAMESTer/_handler.sh    -> HANDLER_DST
    # ROMs are NEVER deployed by this script (user-supplied, license).
    sys.exit(
        "Nothing to deploy: no RBF or emulator binary exists yet.\n"
        "This project is at design/kickoff — see docs/feasibility.md §6.\n"
        "Use --handler-only to install the launch handler, or --dry-run."
    )


def main():
    global HOST
    ap = argparse.ArgumentParser(description="Deploy the MAMESTer MiSTer port.")
    ap.add_argument("--host", default=HOST, help=f"ssh target (default {HOST})")
    ap.add_argument("--dry-run", action="store_true",
                    help="print planned actions without touching the device")
    ap.add_argument("--handler-only", action="store_true",
                    help="install only games/MAMESTer/_handler.sh (works today)")
    args = ap.parse_args()
    HOST = args.host

    print(f"# MAMESTer MiSTer deploy -> {HOST}  (repo: {REPO})")
    if args.handler_only or args.dry_run:
        install_handler(args.dry_run)
        if args.dry_run:
            print("\n(dry run) full deploy would also copy: RBF, emulator ELF. "
                  "Neither exists yet — see docs/feasibility.md.")
        return
    deploy_artifacts(args.dry_run)


if __name__ == "__main__":
    main()
